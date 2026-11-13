#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include "settings_skin.h"
#include "../bmpman/bmpman.h"
#include "../graphics/gr.h"
#include "../graphics/svg_cache.h"
#include "../graphics/ui_draw.h"
#include "../hud/hud_internal.h"
#include "../misc/alpine_settings.h"
#include "../rf/bmpman.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"

namespace
{

// Chrome bitmaps carry their own alpha; the vertex colour tints them, which is how the white
// icon sheets become any palette colour.
constexpr rf::gr::Mode skin_bitmap_mode{
    rf::gr::TEXTURE_SOURCE_CLAMP,
    rf::gr::COLOR_SOURCE_VERTEX_TIMES_TEXTURE,
    rf::gr::ALPHA_SOURCE_VERTEX_TIMES_TEXTURE,
    rf::gr::ALPHA_BLEND_ALPHA,
    rf::gr::ZBUFFER_TYPE_NONE,
    rf::gr::FOG_NOT_ALLOWED,
};

// The stock highlight colours: the red glow of a selected button and a lit list row.
constexpr rf::Color glow_hi{0x7C, 0x1C, 0x0A, 255};
constexpr rf::Color glow_mid{0x4A, 0x0A, 0x06, 255};
constexpr rf::Color glow_lo{0x1A, 0x03, 0x02, 255};
constexpr rf::Color focus_ring_color{0xD0, 0x30, 0x20, 230};

void fill(int x, int y, int w, int h, rf::Color color)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    rf::gr::set_color(color);
    rf::gr::rect(x, y, w, h);
}

void frame(int x, int y, int w, int h, int thickness, rf::Color color)
{
    if (w <= 0 || h <= 0 || thickness <= 0) {
        return;
    }
    rf::gr::set_color(color);
    hud_rect_border(x, y, w, h, thickness);
}

// Salts that derive a widget's internal sub-ids from its own id.
constexpr UiId dropdown_scroll_salt = 0xD30D0001u;
constexpr UiId slider_edit_salt = 0x5D1E0001u;
constexpr UiId stepper_up_salt = 0x57E9A001u;
constexpr UiId stepper_down_salt = 0x57E9A002u;
constexpr UiId stepper_edit_salt = 0x57E9A003u;
constexpr UiId swatch_edit_salt = 0xC0107001u;

UiId sub_id(UiId id, UiId salt)
{
    return id ? (id ^ salt) : 0;
}

int font_h(int font)
{
    return rf::gr::get_font_height(font);
}

int scaled_px(float scale, float reference_px)
{
    return std::max(1, static_cast<int>(reference_px * scale + 0.5f));
}

// Flat bands standing in for a vertical gradient; the engine has no gradient primitive.
void band_fill(int x, int y, int w, int h, const rf::Color* colors, const float* stops, int count)
{
    for (int i = 0; i < count; ++i) {
        const int y0 = y + static_cast<int>(static_cast<float>(h) * stops[i]);
        const int y1 = y + static_cast<int>(static_cast<float>(h) * stops[i + 1]);
        fill(x, y0, w, y1 - y0, colors[i]);
    }
}

void bevel(int x, int y, int w, int h, int t, rf::Color hi, rf::Color lo)
{
    if (w <= 2 * t || h <= 2 * t) {
        return;
    }
    fill(x + t, y + t, w - 2 * t, t, hi);
    fill(x + t, y + t, t, h - 2 * t, hi);
    fill(x + t, y + h - 2 * t, w - 2 * t, t, lo);
    fill(x + w - 2 * t, y + t, t, h - 2 * t, lo);
}

void lamp_dot(const UiContext& ctx, int cx, int cy, float size, rf::Color color, bool lit)
{
    const float s = std::max(2.0f, size * ctx.scale());
    const float r = s * 0.5f;
    const rf::Color dark{static_cast<rf::ubyte>(color.red / 3), static_cast<rf::ubyte>(color.green / 3),
                         static_cast<rf::ubyte>(color.blue / 3), color.alpha};
    ui_rounded_rect(static_cast<float>(cx) - r, static_cast<float>(cy) - r, s, s, r,
                    lit ? color : dark);
}

bool text_focused(const UiContext& ctx, UiId id)
{
    return ctx.focused_text_id() == id;
}

bool focus_shown(const UiContext& ctx, UiId id)
{
    return id != 0 && ctx.focus_visible() && ctx.focus_id() == id;
}

void draw_focus_if_focused(const UiContext& ctx, const UiRect& d, UiId id)
{
    if (focus_shown(ctx, id)) {
        skin_focus_ring(ctx, d.x, d.y, d.w, d.h);
    }
}

// Typing into a numeric well with the kit's text machinery, in the units the well displays: a 0..1
// value shown as a percentage takes "35". The commit clamps to [min_value, max_value]. `activated`
// (Enter on the owning widget) opens it like a click. Hit-test only.
void numeric_entry(UiContext& ctx, const UiRect& well, UiId edit_id, float& value, const UiNumberFormat& fmt,
                   bool integral, float min_value, float max_value, int font, bool enabled, bool activated)
{
    const bool editing = text_focused(ctx, edit_id);
    std::string formatted = ui_format_number(value, fmt);
    const UiTextInputResult res = ui_text_input(ctx, well, edit_id, editing ? ctx.text_buffer() : formatted, 12,
                                                ui_char_filter_numeric, nullptr, font, enabled);
    if (!res.gained_focus) {
        if (!activated || !enabled || editing) {
            return;
        }
        ctx.focus_text(edit_id);
    }
    const float scale = fmt.display_scale != 0.0f ? fmt.display_scale : 1.0f;
    const float shown = value * scale;
    ctx.text_buffer() = integral ? std::format("{}", static_cast<int>(std::lround(shown))) : std::format("{:g}", shown);
    float* const target = &value;
    // The prefill is rounded for display, so an untouched box must not write it back.
    ctx.set_text_commit_callback([c = &ctx, target, min_value, max_value, integral, scale,
                                  prefill = ctx.text_buffer()](bool commit) {
        if (!commit || c->text_buffer() == prefill) {
            return;
        }
        try {
            float parsed = std::stof(c->text_buffer()) / scale;
            if (integral) {
                parsed = std::round(parsed);
            }
            if (std::isfinite(parsed)) {
                *target = std::clamp(parsed, min_value, max_value);
            }
        }
        catch (const std::exception&) {
            // Keep the value that was already there.
        }
    });
    ui_play_click();
}

// One sub-rect of a chrome sheet, drawn 1:1. `sheet_w`/`sheet_h` are the sheet's authored size;
// the sheet is rasterised at that size times the current scale, and `src_*` are authored pixels.
void skin_sheet_blit(const UiContext& ctx, const char* name, float sheet_w, float sheet_h, int x,
                     int y, float src_x, float src_y, float src_w, float src_h)
{
    const int bw = skin_px(ctx, sheet_w);
    const int bh = skin_px(ctx, sheet_h);
    const int bmh = svg_bitmap(name, bw, bh);
    if (bmh == -1) {
        return;
    }
    const int sx = static_cast<int>(src_x * ctx.scale() + 0.5f);
    const int sy = static_cast<int>(src_y * ctx.scale() + 0.5f);
    const int sw = std::min(skin_px(ctx, src_w), bw - sx);
    const int sh = std::min(skin_px(ctx, src_h), bh - sy);
    if (sw <= 0 || sh <= 0) {
        return;
    }
    rf::gr::set_color(255, 255, 255, 255);
    rf::gr::bitmap_scaled(bmh, x, y, sw, sh, sx, sy, sw, sh, false, false, skin_bitmap_mode);
}

void stretch_bitmap(int bm, const UiRect& d, rf::Color tint, bool flip_y)
{
    if (bm == -1 || d.w <= 0 || d.h <= 0) {
        return;
    }
    int bw = 0;
    int bh = 0;
    rf::bm::get_dimensions(bm, &bw, &bh);
    rf::gr::set_color(tint);
    rf::gr::bitmap_scaled(bm, d.x, d.y, d.w, d.h, 0, 0, bw, bh, false, flip_y, skin_bitmap_mode);
}

uint32_t argb(rf::Color c, float alpha)
{
    const auto a = static_cast<uint32_t>(std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
    return (a << 24) | (static_cast<uint32_t>(c.red) << 16) | (static_cast<uint32_t>(c.green) << 8) | c.blue;
}

rf::Color mix(rf::Color a, rf::Color b, float t)
{
    const auto lerp = [t](rf::ubyte x, rf::ubyte y) {
        return static_cast<rf::ubyte>(std::lround(static_cast<float>(x) + (static_cast<float>(y) - x) * t));
    };
    return rf::Color{lerp(a.red, b.red), lerp(a.green, b.green), lerp(a.blue, b.blue), 255};
}

int glow_bar_bitmap()
{
    static const int bm = bm_create_procedural(256, 4, [](int x, int) {
        constexpr rf::Color tail{0x2A, 0x04, 0x03, 255};
        const float t = static_cast<float>(x) / 255.0f;
        if (t < 0.55f) {
            const float k = t / 0.55f;
            return argb(mix({0x6A, 0x14, 0x08, 255}, glow_mid, k), 0.95f + (0.7f - 0.95f) * k);
        }
        const float k = (t - 0.55f) / 0.45f;
        return argb(mix(glow_mid, tail, k), 0.7f * (1.0f - k));
    });
    return bm;
}

int red_glow_bitmap()
{
    constexpr int size = 64;
    static const int bm = bm_create_procedural(size, size, [](int x, int y) {
        const float dx = (static_cast<float>(x) + 0.5f) / size - 0.5f;
        const float dy = (static_cast<float>(y) + 0.5f) / size - 0.5f;
        const float d = std::min(1.0f, std::sqrt(dx * dx + dy * dy) / 0.62f);
        const rf::Color c = d < 0.6f ? mix(glow_hi, glow_mid, d / 0.6f) : mix(glow_mid, glow_lo, (d - 0.6f) / 0.4f);
        return argb(c, 1.0f);
    });
    return bm;
}

// Repeatable 0..1 noise from integer lattice coordinates.
float lattice_noise(int x, int y)
{
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>(h ^ (h >> 16)) / 4294967295.0f;
}

// Brushed steel after stock's panel plates: fine horizontal grain over broad diagonal sheens.
// Stretched over the panel's content area.
int brushed_steel_bitmap()
{
    constexpr int w = 1024;
    constexpr int h = 512;
    static const int bm = bm_create_procedural(w, h, [](int x, int y) {
        // Grain: every pixel row is its own streak, smooth along x and unrelated to its neighbours.
        const float gx = static_cast<float>(x) / 40.0f;
        const int gi = static_cast<int>(gx);
        float gf = gx - static_cast<float>(gi);
        gf = gf * gf * (3.0f - 2.0f * gf);
        const float streak = lattice_noise(gi, y) + (lattice_noise(gi + 1, y) - lattice_noise(gi, y)) * gf;
        const float grain = (lattice_noise(0, y + 7919) - 0.5f) * 0.035f + (streak - 0.5f) * 0.03f;
        // Sheens: a wide bright band from the upper left and a fainter one lower right, over a
        // ground that darkens toward the bottom.
        const float u = static_cast<float>(x) / w * 0.75f + static_cast<float>(y) / h * 0.55f;
        const auto band = [u](float centre, float width) {
            const float k = (u - centre) / width;
            return std::exp(-k * k);
        };
        const float level = 0.64f - 0.06f * static_cast<float>(y) / h + 0.13f * band(0.32f, 0.16f)
                          + 0.07f * band(0.98f, 0.12f) + grain;
        const auto channel = [](float v) {
            return static_cast<rf::ubyte>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
        };
        return argb(rf::Color{channel(level), channel(level), channel(level + 0.025f), 255}, 1.0f);
    });
    return bm;
}

// Brushed steel over a rect at the texture's own proportions: a long thin bar takes a slice of it
// rather than the whole texture squashed, which would lose the grain. `tint` darkens it.
void brushed_fill(int x, int y, int w, int h, rf::Color tint)
{
    const int bm = brushed_steel_bitmap();
    if (bm == -1 || w <= 0 || h <= 0) {
        return;
    }
    int bw = 0;
    int bh = 0;
    rf::bm::get_dimensions(bm, &bw, &bh);
    int sw = bw;
    int sh = std::max(1, bw * h / w);
    if (sh > bh) {
        sh = bh;
        sw = std::max(1, bh * w / h);
    }
    rf::gr::set_color(tint);
    rf::gr::bitmap_scaled(bm, x, y, w, h, 0, 0, sw, sh, false, false, skin_bitmap_mode);
}

void red_glow(const UiRect& d)
{
    stretch_bitmap(red_glow_bitmap(), d, rf::Color{255, 255, 255, 255}, false);
}

// Scale-taking forms, for the overlay lambdas that outlive the context reference.
void led(float scale, int cx, int cy, float w_ref, float h_ref, bool lit)
{
    const float w = std::max(2.0f, w_ref * scale);
    const float h = std::max(3.0f, h_ref * scale);
    const float x = static_cast<float>(cx) - w * 0.5f;
    const float y = static_cast<float>(cy) - h * 0.5f;
    if (lit) {
        ui_rounded_rect(x - w * 1.1f, y - h * 0.2f, w * 3.2f, h * 1.4f, w * 1.6f, rf::Color{0xFF, 0x30, 0x20, 40});
        ui_rounded_rect(x - w * 0.5f, y - h * 0.08f, w * 2.0f, h * 1.16f, w, rf::Color{0xFF, 0x30, 0x20, 64});
    }
    ui_rounded_rect(x, y, w, h, w * 0.5f, rf::Color{0x12, 0x03, 0x02, 255});
    const float inset = std::max(1.0f, scale);
    ui_rounded_rect(x + inset, y + inset, w - 2.0f * inset, h - 2.0f * inset, (w - 2.0f * inset) * 0.5f,
                    lit ? skin::lamp_red : rf::Color{0x3A, 0x0A, 0x08, 255});
    if (lit) {
        const float hw = w * 0.4f;
        ui_rounded_rect(static_cast<float>(cx) - hw * 0.5f, y + h * 0.18f, hw, h * 0.34f, hw * 0.5f,
                        rf::Color{0xFF, 0xD8, 0xD0, 200});
    }
}

void selection_bar(float scale, int x, int y, int w, int h)
{
    stretch_bitmap(glow_bar_bitmap(), UiRect{x, y, w, h}, rf::Color{255, 255, 255, 255}, false);
    led(scale, x + scaled_px(scale, 12.0f), y + h / 2, 7.0f, std::min(26.0f, static_cast<float>(h) * 0.6f / scale),
        true);
}

void slot(float scale, int x, int y, int w, int h)
{
    fill(x, y, w, h, skin::field);
    frame(x, y, w, h, scaled_px(scale, 1.0f), skin::field_edge);
}

rf::Color value_color(bool enabled, bool hot)
{
    if (!enabled) {
        return skin::txt_dim;
    }
    return hot ? skin::txt_hi : skin::txt;
}

rf::Color arrow_color(bool enabled, bool lit)
{
    if (!enabled) {
        return skin::txt_dim;
    }
    return lit ? skin::teal_led : skin::arrow;
}

void skin_steel_plate(const UiContext& ctx, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    static constexpr rf::Color bands[5] = {skin::steel_hi, skin::steel_lit, skin::steel, skin::steel_low,
                                           skin::steel_mid};
    static constexpr float stops[6] = {0.0f, 0.16f, 0.42f, 0.68f, 0.86f, 1.0f};
    band_fill(x, y, w, h, bands, stops, 5);
    const int t = skin_px(ctx, 1.0f);
    frame(x, y, w, h, t, skin::steel_edge);
    bevel(x, y, w, h, t, skin::steel_hi, skin::steel_dark);
}

void skin_bevel_field(const UiContext& ctx, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    const int t = skin_px(ctx, 2.0f);
    fill(x, y, w, h, skin::field);
    fill(x, y, w, t, skin::bevel_hi);
    fill(x, y, t, h, skin::bevel_hi);
    fill(x + t, y + h - t, w - t, t, skin::bevel_lo);
    fill(x + w - t, y + t, t, h - t, skin::bevel_lo);
}

void skin_icon(const char* name, int x, int y, int size, rf::Color tint)
{
    if (size <= 0) {
        return;
    }
    const int bmh = svg_bitmap(name, size, size);
    if (bmh == -1) {
        return;
    }
    rf::gr::set_color(tint);
    rf::gr::bitmap(bmh, x, y, skin_bitmap_mode);
}

enum class SkinArrow
{
    Left,
    Right,
    Up,
    Down,
    UpOutline,
    DownOutline,
};

void skin_arrow(SkinArrow arrow, int x, int y, int size, rf::Color tint)
{
    // af_arrows is a strip of square cells in SkinArrow order, rasterised so one cell is `size`.
    constexpr int cells = 6;
    if (size <= 0) {
        return;
    }
    const int bmh = svg_bitmap("af_arrows", cells * size, size);
    if (bmh == -1) {
        return;
    }
    rf::gr::set_color(tint);
    rf::gr::bitmap_scaled(bmh, x, y, size, size, static_cast<int>(arrow) * size, 0, size, size, false, false,
                          skin_bitmap_mode);
}

struct CyclerParts
{
    UiRect left;
    UiRect right;
    UiRect well;
};

CyclerParts cycler_parts(const UiContext& ctx, const UiRect& r)
{
    const int aw = skin_px(ctx, skin::cycler_arrow);
    const int gap = skin_px(ctx, skin::cycler_gap);
    return CyclerParts{UiRect{r.x, r.y, aw, r.h}, UiRect{r.x + r.w - aw, r.y, aw, r.h},
                       UiRect{r.x + aw + gap, r.y, std::max(0, r.w - 2 * (aw + gap)), r.h}};
}

void draw_cycler_arrows(const UiContext& ctx, const UiRect& dl, const UiRect& dr, bool enabled, bool left_lit,
                        bool right_lit)
{
    // The arrows sit on the panel's steel, so they are dark; a lit one turns teal inside a dark
    // outline, as stock's do.
    const int size = std::min(dl.h, skin_px(ctx, 30.0f));
    const int edge = std::max(1, skin_px(ctx, 2.0f));
    const auto draw = [&](SkinArrow arrow, const UiRect& d, bool lit) {
        const int x = d.x + (d.w - size) / 2;
        const int y = d.y + (d.h - size) / 2;
        if (!enabled) {
            skin_arrow(arrow, x, y, size, skin::ink_dim);
            return;
        }
        skin_arrow(arrow, x - edge, y - edge, size + 2 * edge, skin::ink_status);
        if (lit) {
            skin_arrow(arrow, x, y, size, skin::teal_led);
        }
    };
    draw(SkinArrow::Left, dl, left_lit);
    draw(SkinArrow::Right, dr, right_lit);
}

// A closed dropdown: the value on the left of a well, a down arrow in the cap on its right.
void draw_select_box(const UiContext& ctx, const UiRect& d, std::string_view text, int font, bool enabled,
                     bool active, bool hot)
{
    const int cap_w = skin_px(ctx, skin::dropdown_cap);
    skin_slot(ctx, d.x, d.y, d.w, d.h);
    const int size = std::min(d.h, skin_px(ctx, 26.0f));
    skin_arrow(SkinArrow::Down, d.x + d.w - cap_w + (cap_w - size) / 2, d.y + (d.h - size) / 2, size,
               arrow_color(enabled, active));
    const int tp = skin_px(ctx, 12.0f);
    rf::gr::set_color(value_color(enabled, hot || active));
    const SkinText fitted = skin_fit_text(ctx, text, font, std::max(0, d.w - cap_w - 2 * tp));
    rf::gr::string(d.x + tp, d.y + (d.h - font_h(fitted.font)) / 2, fitted.text.c_str(), fitted.font);
}

constexpr int ramp_texels = 256;

uint32_t ramp_alpha(int i)
{
    return static_cast<uint32_t>(i * 255 / (ramp_texels - 1));
}

int white_fade_right_bitmap()
{
    static const int bm = bm_create_procedural(ramp_texels, 4, [](int x, int) {
        return ((255u - ramp_alpha(x)) << 24) | 0x00FFFFFFu;
    });
    return bm;
}

int white_fade_down_bitmap()
{
    static const int bm = bm_create_procedural(4, ramp_texels, [](int, int y) {
        return (ramp_alpha(y) << 24) | 0x00FFFFFFu;
    });
    return bm;
}

int hue_bitmap()
{
    static const int bm = bm_create_procedural(4, ramp_texels, [](int, int y) {
        const float h = static_cast<float>(y) / static_cast<float>(ramp_texels - 1);
        return 0xFF000000u | (skin_hsva_to_rgba(SkinHsva{h, 1.0f, 1.0f, 1.0f}) >> 8);
    });
    return bm;
}

// Grey checks behind anything translucent, so the alpha of a colour reads.
void checker(const UiContext& ctx, const UiRect& d)
{
    const int cell = skin_px(ctx, 8.0f);
    fill(d.x, d.y, d.w, d.h, rf::Color{0x9A, 0x9A, 0x9A, 255});
    for (int row = 0; row * cell < d.h; ++row) {
        for (int col = row & 1; col * cell < d.w; col += 2) {
            fill(d.x + col * cell, d.y + row * cell, std::min(cell, d.w - col * cell),
                 std::min(cell, d.h - row * cell), rf::Color{0x5C, 0x5C, 0x5C, 255});
        }
    }
}

void picker_well(const UiContext& ctx, const UiRect& d)
{
    frame(d.x - skin_px(ctx, 2.0f), d.y - skin_px(ctx, 2.0f), d.w + 2 * skin_px(ctx, 2.0f),
          d.h + 2 * skin_px(ctx, 2.0f), skin_px(ctx, 2.0f), rf::Color{0, 0, 0, 255});
}

// Horizontal bar across a strip at `t` (0 = top).
void strip_marker(const UiContext& ctx, const UiRect& d, float t)
{
    const int over = skin_px(ctx, 4.0f);
    const int h = skin_px(ctx, 8.0f);
    const int y = d.y + static_cast<int>(t * static_cast<float>(d.h - 1)) - h / 2;
    frame(d.x - over, y, d.w + 2 * over, h, skin_px(ctx, 1.0f), rf::Color{0, 0, 0, 255});
    frame(d.x - over + skin_px(ctx, 1.0f), y + skin_px(ctx, 1.0f), d.w + 2 * over - 2 * skin_px(ctx, 1.0f),
          h - 2 * skin_px(ctx, 1.0f), skin_px(ctx, 2.0f), skin::txt_hi);
}

// A press inside captures the area; the cursor then drives it, clamped to the area, until the
// button is released. `tx` / `ty` run 0..1 across / down.
bool picker_drag(UiContext& ctx, const UiRect& r, UiId id, float& tx, float& ty)
{
    if (ui_hover(ctx, r, id) && ctx.click) {
        ctx.capture(id);
        ui_play_click();
    }
    if (ctx.captured_id() != id) {
        return false;
    }
    if (!ctx.lmb_down) {
        ctx.release_capture();
        return false;
    }
    tx = std::clamp(static_cast<float>(ctx.mx - r.x) / static_cast<float>(std::max(1, r.w - 1)), 0.0f, 1.0f);
    ty = std::clamp(static_cast<float>(ctx.my - r.y) / static_cast<float>(std::max(1, r.h - 1)), 0.0f, 1.0f);
    return true;
}

// Left / Right on a focused picker area.
bool picker_adjust(UiContext& ctx, UiId id, float& value, float step)
{
    const int adjust = ctx.consume_adjust(id);
    if (adjust == 0) {
        return false;
    }
    value = std::clamp(value + step * static_cast<float>(adjust), 0.0f, 1.0f);
    return true;
}

constexpr float picker_key_step = 0.02f;

} // namespace

int skin_px(const UiContext& ctx, float reference_px)
{
    if (reference_px <= 0.0f) {
        return 0;
    }
    return scaled_px(ctx.scale(), reference_px);
}

SkinText skin_fit_text(const UiContext& ctx, std::string_view text, int font, int max_w)
{
    if (text.empty() || max_w <= 0 || rf::gr::get_string_size(text, font).first <= max_w) {
        return SkinText{std::string{text}, font};
    }

    int fitted = font;
    bool bold = false;
    int px = 0;
    if (ui_ttf_font_info(font, bold, px)) {
        // menu_font already clamps to 8 px; below that the atlas is unreadable anyway.
        const int floor_px = std::max(8, std::max(static_cast<int>(std::lround(px * 0.70f)),
                                                  static_cast<int>(std::lround(12.0f * ctx.scale()))));
        for (int size = px - 1; size >= floor_px; --size) {
            const int candidate = ui_ttf_font(bold, size);
            if (candidate < 0) {
                break;
            }
            fitted = candidate;
            if (rf::gr::get_string_size(text, candidate).first <= max_w) {
                return SkinText{std::string{text}, candidate};
            }
        }
    }
    return SkinText{ui_fit_middle(text, max_w, fitted), fitted};
}

int skin_button_width(const UiContext& ctx, const char* text, int font, float min_ref_w)
{
    const int needed = rf::gr::get_string_size(text ? text : "", font).first + 2 * skin_px(ctx, 14.0f);
    return std::max(skin_px(ctx, min_ref_w), needed);
}

UiStyle settings_menu_style()
{
    UiStyle s;
    s.body_text = skin::txt;

    s.toast_bg = skin::popup;
    s.toast_border = skin::border;
    s.toast_text = skin::txt_hi;

    // The scroll region keeps the wheel, the clip window and the drag; skin_scrollbar paints the
    // gutter, so the kit's own bar is made invisible rather than removed.
    s.scrollbar = rf::Color{0, 0, 0, 0};
    s.scrollbar_hover = rf::Color{0, 0, 0, 0};
    s.scrollbar_track = rf::Color{0, 0, 0, 0};
    s.scrollbar_w = skin::scrollbar_w;
    s.scrollbar_w_min = 4;
    s.scrollbar_cap = skin::scrollbar_w;
    return s;
}

// ---------------------------------------------------------------------------
// Surfaces
// ---------------------------------------------------------------------------

void skin_brushed_plate(const UiContext& ctx, int x, int y, int w, int h, bool shaded)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    brushed_fill(x, y, w, h, shaded ? rf::Color{0xB8, 0xB8, 0xBE, 255} : rf::Color{255, 255, 255, 255});
    const int t = skin_px(ctx, 1.0f);
    frame(x, y, w, h, t, skin::steel_edge);
    bevel(x, y, w, h, t, shaded ? skin::steel_lit : skin::steel_hi, skin::steel_dark);
}

void skin_steel_field(const UiContext& ctx, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    brushed_fill(x, y, w, h, rf::Color{255, 255, 255, 255});
    // Recessed: dark top and left edges, bright bottom and right.
    const int t = std::max(1, skin_px(ctx, 1.0f));
    fill(x, y, w, t, skin::steel_dark);
    fill(x, y, t, h, skin::steel_dark);
    fill(x, y + h - t, w, t, skin::steel_hi);
    fill(x + w - t, y, t, h, skin::steel_hi);
}

void skin_ridge(const UiContext& ctx, int x, int y, int w)
{
    const int t = std::max(1, skin_px(ctx, 1.0f));
    fill(x, y, w, t, skin::steel_mid);
    fill(x, y + t, w, t, skin::steel_hi);
}

void skin_screw(const UiContext& ctx, int cx, int cy, float size_ref)
{
    const float d = size_ref * ctx.scale();
    const float r = d * 0.5f;
    const float x = static_cast<float>(cx) - r;
    const float y = static_cast<float>(cy) - r;
    const float width = std::max(1.0f, 1.5f * ctx.scale());
    ui_rounded_rect(x, y, d, d, r, skin::steel_lit);
    ui_rounded_rect_border(x, y, d, d, r, width, skin::steel_dark);
    const float arm = r * 0.55f;
    ui_aa_line(static_cast<float>(cx) - arm, static_cast<float>(cy), static_cast<float>(cx) + arm,
               static_cast<float>(cy), width, skin::steel_dark);
    ui_aa_line(static_cast<float>(cx), static_cast<float>(cy) - arm, static_cast<float>(cx),
               static_cast<float>(cy) + arm, width, skin::steel_dark);
}

void skin_slot(const UiContext& ctx, int x, int y, int w, int h)
{
    slot(ctx.scale(), x, y, w, h);
}

void skin_led(const UiContext& ctx, int cx, int cy, float w_ref, float h_ref, bool lit)
{
    led(ctx.scale(), cx, cy, w_ref, h_ref, lit);
}

void skin_selection_bar(const UiContext& ctx, int x, int y, int w, int h)
{
    selection_bar(ctx.scale(), x, y, w, h);
}

void skin_focus_ring(const UiContext& ctx, int x, int y, int w, int h)
{
    const int out = skin_px(ctx, 3.0f);
    ui_rounded_rect_border(static_cast<float>(x - out), static_cast<float>(y - out), static_cast<float>(w + 2 * out),
                           static_cast<float>(h + 2 * out), 2.0f * ctx.scale(),
                           static_cast<float>(skin_px(ctx, 2.0f)), focus_ring_color);
}

void skin_sized_chrome(const char* name, const UiRect& r)
{
    if (r.w <= 0 || r.h <= 0) {
        return;
    }
    const int bmh = svg_bitmap(name, r.w, r.h);
    if (bmh == -1) {
        return;
    }
    rf::gr::set_color(255, 255, 255, 255);
    rf::gr::bitmap(bmh, r.x, r.y, skin_bitmap_mode);
}

void skin_popup(const UiContext& ctx, const UiRect& dialog, const UiRect& band, const char* title, int font)
{
    fill(dialog.x, dialog.y, dialog.w, dialog.h, skin::popup);
    frame(dialog.x, dialog.y, dialog.w, dialog.h, skin_px(ctx, 1.0f), skin::border);
    fill(band.x, band.y, band.w, band.h, skin::field);
    fill(band.x, band.y + band.h - skin_px(ctx, 1.0f), band.w, skin_px(ctx, 1.0f), skin::border_dim);
    skin_led(ctx, band.x + skin_px(ctx, 18.0f), band.y + band.h / 2, 7.0f, 24.0f, true);
    rf::gr::set_color(skin::txt_hi);
    rf::gr::string(band.x + skin_px(ctx, 36.0f), band.y + (band.h - font_h(font)) / 2, title ? title : "", font);
}

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------

bool skin_button(UiContext& ctx, const UiRect& r, const char* text, int font, SkinButton kind,
                 bool enabled, UiId id)
{
    const bool hovered = enabled && ui_hover(ctx, r, id);
    if (hovered) {
        ctx.note_hovered(id);
    }
    if (enabled) {
        ctx.register_focusable(id, UiFocusKind::Activate);
    }

    if (ctx.draw) {
        const UiRect d = ui_draw_rect(ctx, r);
        const bool lit = enabled && (hovered || focus_shown(ctx, id));
        if (lit) {
            red_glow(d);
        }
        else {
            fill(d.x, d.y, d.w, d.h, skin::field);
        }
        frame(d.x, d.y, d.w, d.h, skin_px(ctx, 1.0f), skin::field_edge);
        const int pad = skin_px(ctx, 12.0f);
        int text_x = d.x + d.w / 2;
        int text_w = d.w - 2 * pad;
        if (kind == SkinButton::Danger) {
            const int lamp_w = skin_px(ctx, 30.0f);
            skin_led(ctx, d.x + skin_px(ctx, 18.0f), d.y + d.h / 2, 7.0f, std::min(24.0f, d.h * 0.6f / ctx.scale()),
                     enabled);
            text_x = d.x + lamp_w + (d.w - lamp_w) / 2;
            text_w = d.w - lamp_w - 2 * pad;
        }
        rf::gr::set_color(!enabled ? skin::txt_dim : (lit ? skin::txt_hi : skin::txt));
        const SkinText label = skin_fit_text(ctx, text ? text : "", font, text_w);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, text_x,
                               d.y + (d.h - font_h(label.font)) / 2, label.text.c_str(), label.font);
        return false;
    }

    if (enabled && ctx.consume_activation(id)) {
        return true;
    }
    return hovered && ctx.click;
}

int skin_menu_button_label_w(const UiContext& ctx, int w, int label_inset)
{
    // The visible well left of the end cap, less a pad each side.
    return std::max(0, w - label_inset - skin_px(ctx, skin::menu_cap_w) - 2 * skin_px(ctx, 12.0f));
}

bool skin_menu_button(UiContext& ctx, const UiRect& r, int hidden_w, int label_inset, const char* label, int font,
                      bool selected, bool arrow, UiId id)
{
    // The part under the bar still takes clicks, as stock's buttons do.
    const bool hovered = ui_hover(ctx, UiRect{r.x - hidden_w, r.y, r.w + hidden_w, r.h}, id);
    if (hovered) {
        ctx.note_hovered(id);
    }
    ctx.register_focusable(id, UiFocusKind::Activate);

    if (ctx.draw) {
        const bool focused = focus_shown(ctx, id);
        const bool lit = selected || hovered || focused;
        const UiRect d = ui_draw_rect(ctx, r);
        const int cap_w = skin_px(ctx, skin::menu_cap_w);
        const int cap_x = d.x + d.w - cap_w;
        const int x0 = d.x - hidden_w;
        // The rails run on under the cap body, as on stock button.tga.
        const int rail_h = std::max(2, d.h * 12 / 100);
        const UiRect well{x0, d.y + rail_h, cap_x - x0, d.h - 2 * rail_h};
        if (lit) {
            red_glow(well);
        }
        else {
            fill(well.x, well.y, well.w, well.h, rf::Color{0x12, 0x07, 0x03, 158});
        }
        const int rail_w = cap_x + skin_px(ctx, skin::menu_cap_body_w) - x0;
        skin_steel_plate(ctx, x0, d.y, rail_w, rail_h);
        skin_steel_plate(ctx, x0, d.y + d.h - rail_h, rail_w, rail_h);
        // af_menu_cap stacks four 72x78 caps 86 apart: arrow, arrow lit, plain, plain lit.
        const int state = (arrow ? 0 : 2) + (lit ? 1 : 0);
        skin_sheet_blit(ctx, "af_menu_cap", skin::menu_cap_w, 336.0f * skin::menu_cap_scale, cap_x, d.y, 0.0f,
                        86.0f * skin::menu_cap_scale * static_cast<float>(state), skin::menu_cap_w,
                        skin::menu_button_h);
        // The selected button is already lit, so keyboard focus on it needs its own mark.
        if (focused && selected) {
            skin_focus_ring(ctx, d.x, d.y, d.w, d.h);
        }

        const int well_cx = (d.x + label_inset + cap_x) / 2;
        const int max_w = skin_menu_button_label_w(ctx, d.w, label_inset);
        const std::string_view text{label ? label : ""};
        rf::gr::set_color(lit ? skin::txt_hi : skin::txt);
        if (rf::gr::get_string_size(text, font).first <= max_w) {
            rf::gr::string_aligned(rf::gr::ALIGN_CENTER, well_cx, d.y + (d.h - font_h(font)) / 2,
                                   std::string{text}.c_str(), font);
            return false;
        }
        // Two lines, broken at the space that leaves the wider half narrowest.
        std::size_t split = std::string_view::npos;
        int best = std::numeric_limits<int>::max();
        for (std::size_t i = text.find(' '); i != std::string_view::npos; i = text.find(' ', i + 1)) {
            const int wider = std::max(rf::gr::get_string_size(text.substr(0, i), font).first,
                                       rf::gr::get_string_size(text.substr(i + 1), font).first);
            if (wider < best) {
                best = wider;
                split = i;
            }
        }
        if (split == std::string_view::npos) {
            const SkinText fitted = skin_fit_text(ctx, text, font, max_w);
            rf::gr::string_aligned(rf::gr::ALIGN_CENTER, well_cx, d.y + (d.h - font_h(fitted.font)) / 2,
                                   fitted.text.c_str(), fitted.font);
            return false;
        }
        const SkinText top = skin_fit_text(ctx, text.substr(0, split), font, max_w);
        const SkinText bottom = skin_fit_text(ctx, text.substr(split + 1), font, max_w);
        const int line_h = std::max(font_h(top.font), font_h(bottom.font));
        const int y0 = d.y + (d.h - 2 * line_h) / 2;
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, well_cx, y0, top.text.c_str(), top.font);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, well_cx, y0 + line_h, bottom.text.c_str(), bottom.font);
        return false;
    }

    if (ctx.consume_activation(id)) {
        return true;
    }
    return hovered && ctx.click;
}

bool skin_icon_button(UiContext& ctx, const UiRect& r, const char* icon, rf::Color tint,
                      rf::Color tint_hover, UiId id)
{
    const bool hovered = ui_hover(ctx, r, id);
    if (hovered) {
        ctx.note_hovered(id);
    }
    ctx.register_focusable(id, UiFocusKind::Activate);

    if (ctx.draw) {
        const UiRect d = ui_draw_rect(ctx, r);
        const int size = std::min(d.w, d.h);
        skin_icon(icon, d.x + (d.w - size) / 2, d.y + (d.h - size) / 2, size,
                  hovered || focus_shown(ctx, id) ? tint_hover : tint);
        draw_focus_if_focused(ctx, d, id);
        return false;
    }

    if (ctx.consume_activation(id)) {
        return true;
    }
    return hovered && ctx.click;
}

int skin_cycler(UiContext& ctx, const UiRect& r, const char* value, int font, bool enabled, UiId id, bool hot)
{
    const auto [left, right, plate] = cycler_parts(ctx, r);

    const bool hl = enabled && ui_hover(ctx, left, id);
    const bool hr = enabled && ui_hover(ctx, right, id);
    const bool hm = enabled && ui_hover(ctx, plate, id);
    if (hl || hr || hm) {
        ctx.note_hovered(id);
    }
    if (enabled) {
        ctx.register_focusable(id, UiFocusKind::Adjustable);
    }

    if (ctx.draw) {
        const UiRect dl = ui_draw_rect(ctx, left);
        const UiRect dr = ui_draw_rect(ctx, right);
        const UiRect dp = ui_draw_rect(ctx, plate);
        const bool active = focus_shown(ctx, id);
        draw_cycler_arrows(ctx, dl, dr, enabled, hl || active, hr || hm || active);
        skin_slot(ctx, dp.x, dp.y, dp.w, dp.h);
        rf::gr::set_color(value_color(enabled, hot || hl || hr || hm || active));
        const SkinText fitted =
            skin_fit_text(ctx, value ? value : "", font, dp.w - skin_px(ctx, 20.0f));
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, dp.x + dp.w / 2,
                               dp.y + (dp.h - font_h(fitted.font)) / 2, fitted.text.c_str(),
                               fitted.font);
        return 0;
    }

    if (!enabled) {
        return 0;
    }
    if (const int adjust = ctx.consume_adjust(id); adjust != 0) {
        return adjust;
    }
    if (ctx.consume_activation(id)) {
        return 1;
    }
    if (ctx.rclick && (hl || hr || hm)) {
        return -1;
    }
    if (!ctx.click) {
        return 0;
    }
    if (hl) {
        return -1;
    }
    return (hr || hm) ? 1 : 0;
}

bool skin_dropdown(UiContext& ctx, const UiRect& r, const std::vector<std::string>& items, int& sel,
                   int font, bool enabled, UiId id, bool hot)
{
    const int row_h = font_h(font) + skin_px(ctx, 10.0f);
    const int count = static_cast<int>(items.size());
    const int pad = skin_px(ctx, 3.0f);
    // A long enum would run off the canvas, so the open list is capped to a third of the screen
    // and scrolled instead.
    // Sized against the screen, not the clip: the clip is the scroll viewport in the draw pass but
    // the whole screen in the hit-test pass, and both passes must lay the list out identically.
    const int screen_h = rf::gr::screen_height();
    const int fit_rows = std::max(4, screen_h / std::max(1, row_h) / 3);
    const int shown_rows = std::max(1, std::min(count, fit_rows));
    const int max_first = std::max(0, count - shown_rows);
    const int list_h = shown_rows * row_h + 2 * pad;
    const int below = r.y + r.h + skin_px(ctx, 2.0f);
    const bool fits = below + list_h <= screen_h;
    const UiRect list{r.x, fits ? below : std::max(0, r.y - list_h), r.w, list_h};

    bool open = ctx.popup_open_for(id);
    if (open && !enabled && !ctx.draw) {
        ctx.close_popup();
        open = false;
    }
    const bool hovered = enabled && ui_hover(ctx, r, id);
    if (hovered) {
        ctx.note_hovered(id);
    }
    if (enabled) {
        ctx.register_focusable(id, UiFocusKind::Adjustable);
    }

    bool changed = false;
    float& list_scroll = ctx.scroll_offset(sub_id(id, dropdown_scroll_salt));
    // Opening the list parks the selection in the middle of the window.
    const auto centre_on_selection = [&] {
        list_scroll = static_cast<float>(std::clamp(sel - shown_rows / 2, 0, max_first));
    };

    if (!ctx.draw && enabled) {
        if (open && ctx.wheel != 0 && ui_point_in(ctx.mx, ctx.my, list)) {
            list_scroll += ctx.wheel > 0 ? -1.0f : 1.0f;
        }
        if (open) {
            list_scroll += ctx.analog_scroll / static_cast<float>(std::max(1, row_h));
        }
        if (ctx.consume_activation(id)) {
            if (open) {
                ctx.close_popup();
            }
            else {
                centre_on_selection();
                ctx.open_popup(id);
            }
            ui_play_click();
        }
        else if (const int adjust = ctx.consume_adjust(id); adjust != 0 && count > 0) {
            sel = std::clamp(sel + adjust, 0, count - 1);
            changed = true;
            if (open) {
                centre_on_selection();
            }
        }
        else if (ctx.click) {
            if (open) {
                if (ui_point_in(ctx.mx, ctx.my, list)) {
                    const int index = (ctx.my - list.y - pad) / row_h
                        + std::clamp(static_cast<int>(list_scroll), 0, max_first);
                    if (index >= 0 && index < count) {
                        sel = index;
                        changed = true;
                    }
                    ui_play_click();
                }
                ctx.close_popup();
            }
            else if (hovered) {
                centre_on_selection();
                ctx.open_popup(id);
                ui_play_click();
            }
        }
        list_scroll = std::clamp(list_scroll, 0.0f, static_cast<float>(max_first));
    }

    if (ctx.draw) {
        const UiRect d = ui_draw_rect(ctx, r);
        draw_select_box(ctx, d, (sel >= 0 && sel < count) ? std::string_view{items[sel]} : std::string_view{"-"},
                        font, enabled, hovered || open || focus_shown(ctx, id), hot);

        if (open && count > 0) {
            const int selected = sel;
            const int first = std::clamp(static_cast<int>(list_scroll), 0, max_first);
            const std::vector<std::string> rows{items.begin() + first,
                                                items.begin() + first + shown_rows};
            const bool scrollable = count > shown_rows;
            const float bar_t = max_first > 0 ? static_cast<float>(first) / static_cast<float>(max_first)
                                              : 0.0f;
            const float bar_frac = static_cast<float>(shown_rows) / static_cast<float>(count);
            const int mx = ctx.mx;
            const int my = ctx.my;
            const float scale = ctx.scale();
            ctx.push_overlay([list, rows, selected, first, scrollable, bar_t, bar_frac, font, row_h,
                              pad, mx, my, scale]() {
                const rf::Color opaque{skin::popup.red, skin::popup.green, skin::popup.blue, 255};
                fill(list.x, list.y, list.w, list.h, opaque);
                frame(list.x, list.y, list.w, list.h, scaled_px(scale, 1.0f), skin::border);
                int save_x = 0, save_y = 0, save_w = 0, save_h = 0;
                rf::gr::get_clip(&save_x, &save_y, &save_w, &save_h);
                rf::gr::set_clip(list.x + pad, list.y + pad, list.w - 2 * pad, list.h - 2 * pad);
                for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
                    const int iy = list.y + pad + i * row_h;
                    const int dy = i * row_h;
                    const bool row_hovered =
                        ui_point_in(mx, my, UiRect{list.x, iy, list.w, row_h});
                    const bool row_selected = first + i == selected;
                    if (row_selected) {
                        selection_bar(scale, 0, dy, list.w - 2 * pad, row_h);
                    }
                    rf::gr::set_color(row_selected || row_hovered ? skin::txt_hi : skin::txt);
                    rf::gr::string(scaled_px(scale, 28.0f),
                                   dy + (row_h - rf::gr::get_font_height(font)) / 2,
                                   rows[i].c_str(), font);
                }
                rf::gr::set_clip(save_x, save_y, save_w, save_h);
                if (scrollable) {
                    const int bar_w = std::max(2, static_cast<int>(4.0f * scale));
                    const int track_h = list.h - 2 * pad;
                    const int thumb_h = std::max(bar_w * 2,
                                                 static_cast<int>(bar_frac * static_cast<float>(track_h)));
                    const int bx = list.x + list.w - pad - bar_w;
                    fill(bx, list.y + pad, bar_w, track_h, rf::Color{0x18, 0x18, 0x18, 255});
                    fill(bx,
                         list.y + pad
                             + static_cast<int>(bar_t * static_cast<float>(std::max(0, track_h - thumb_h))),
                         bar_w, thumb_h, skin::steel_mid);
                }
            });
        }
    }

    return changed;
}

bool skin_select_box(UiContext& ctx, const UiRect& r, const char* text, int font, bool enabled, UiId id, bool hot)
{
    const bool hovered = enabled && ui_hover(ctx, r, id);
    if (hovered) {
        ctx.note_hovered(id);
    }
    if (enabled) {
        ctx.register_focusable(id, UiFocusKind::Activate);
    }

    if (ctx.draw) {
        draw_select_box(ctx, ui_draw_rect(ctx, r), text ? text : "", font, enabled,
                        hovered || focus_shown(ctx, id), hot);
        return false;
    }

    if (enabled && ctx.consume_activation(id)) {
        return true;
    }
    return hovered && ctx.click;
}

int skin_slider_readout_w(const UiContext& ctx, float min_value, float max_value, const UiNumberFormat& fmt,
                          int font)
{
    int text_w = fmt.zero_label ? rf::gr::get_string_size(fmt.zero_label, font).first : 0;
    for (const float v : {min_value, max_value}) {
        std::string text = ui_format_number(v, fmt);
        std::replace_if(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }, '8');
        text_w = std::max(text_w, rf::gr::get_string_size(text, font).first);
    }
    return std::max(skin_px(ctx, skin::readout_w), text_w + 2 * skin_px(ctx, 8.0f));
}

bool skin_slider(UiContext& ctx, const UiRect& r, float& value, float min_value, float max_value,
                 float step, float nudge, const UiNumberFormat& fmt, int font, int readout_w, bool enabled,
                 UiId id, bool hot)
{
    const int rw = readout_w;
    const int rh = skin_px(ctx, skin::readout_h);
    const int gap = skin_px(ctx, 12.0f);
    const int th = skin_px(ctx, skin::slider_track_h);
    // The track takes the rest of `r`, less the half nub that overhangs its left end at the minimum.
    const int tw = std::max(skin_px(ctx, 60.0f), r.w - rw - gap - skin_px(ctx, skin::slider_thumb_w) / 2);
    const UiRect readout{r.x + r.w - rw, r.y + (r.h - rh) / 2, rw, rh};
    const UiRect track{readout.x - gap - tw, r.y + (r.h - th) / 2, tw, th};
    // The nub overhangs the track, so grabbing is tested against the taller band.
    const int grab_h = skin_px(ctx, skin::slider_thumb_h);
    const UiRect grab{track.x, r.y + (r.h - grab_h) / 2, track.w, grab_h};

    const bool hovered = enabled && ui_hover(ctx, grab, id);
    if (hovered) {
        ctx.note_hovered(id);
    }
    if (enabled) {
        ctx.register_focusable(id, UiFocusKind::Adjustable);
    }

    const float span = max_value - min_value;
    const float before = value;

    const auto snap = [&](float v) {
        float out = v;
        if (step > 0.0f) {
            out = min_value + std::round((out - min_value) / step) * step;
        }
        return std::clamp(out, min_value, max_value);
    };

    bool activated = false;
    if (!ctx.draw && enabled) {
        activated = ctx.consume_activation(id);
        // A stored value can sit outside the track's range; a step must not clamp it the wrong way.
        if (const int adjust = ctx.consume_adjust(id);
            adjust != 0 && !(adjust > 0 && value >= max_value) && !(adjust < 0 && value <= min_value)) {
            const float delta = step > 0.0f ? step : (nudge > 0.0f ? nudge : span / 20.0f);
            value = snap(value + delta * static_cast<float>(adjust));
        }
        if (hovered && ctx.click) {
            ctx.capture(id);
        }
        if (ctx.captured_id() == id) {
            if (ctx.lmb_down) {
                const float t = std::clamp(static_cast<float>(ctx.mx - track.x)
                                               / static_cast<float>(std::max(1, track.w)),
                                           0.0f, 1.0f);
                value = snap(min_value + t * span);
            }
            else {
                ctx.release_capture();
            }
        }
    }

    // The readout takes a typed value too. It is not held to the track's range: the setting's own
    // limits apply when it is stored, so a value beyond the track can still be entered.
    const UiId edit_id = sub_id(id, slider_edit_salt);
    const bool editing = text_focused(ctx, edit_id);
    if (!ctx.draw) {
        numeric_entry(ctx, readout, edit_id, value, fmt, false, std::numeric_limits<float>::lowest(),
                      std::numeric_limits<float>::max(), font, enabled, activated);
    }

    const float t = span > 0.0f ? std::clamp((value - min_value) / span, 0.0f, 1.0f) : 0.0f;

    if (ctx.draw) {
        const UiRect d = ui_draw_rect(ctx, track);
        skin_slot(ctx, d.x, d.y, d.w, d.h);
        const int thumb_w = skin_px(ctx, skin::slider_thumb_w);
        const int thumb_h = skin_px(ctx, skin::slider_thumb_h);
        const int thumb_x = d.x + static_cast<int>(t * static_cast<float>(d.w)) - thumb_w / 2;
        const int thumb_y = d.y + (d.h - thumb_h) / 2;
        const bool dragging = ctx.captured_id() == id;
        skin_steel_plate(ctx, thumb_x, thumb_y, thumb_w, thumb_h);
        if (dragging) {
            const int core = std::max(2, skin_px(ctx, 4.0f));
            fill(thumb_x + (thumb_w - core) / 2, thumb_y + skin_px(ctx, 3.0f), core,
                 thumb_h - 2 * skin_px(ctx, 3.0f), skin::teal_led);
        }
        if (!enabled) {
            fill(thumb_x, thumb_y, thumb_w, thumb_h, rf::Color{0, 0, 0, 140});
        }

        const UiRect dro = ui_draw_rect(ctx, readout);
        if (editing) {
            skin_bevel_field(ctx, dro.x, dro.y, dro.w, dro.h);
        }
        else {
            skin_slot(ctx, dro.x, dro.y, dro.w, dro.h);
        }
        rf::gr::set_color(editing ? skin::txt_hi : value_color(enabled, hot || hovered || dragging));
        const int text_pad = skin_px(ctx, 8.0f);
        const SkinText shown = skin_fit_text(ctx, editing ? ctx.text_buffer() + "_" : ui_format_number(value, fmt),
                                             font, std::max(0, dro.w - 2 * text_pad));
        rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, dro.x + dro.w - text_pad,
                               dro.y + (dro.h - font_h(shown.font)) / 2, shown.text.c_str(), shown.font);
        return false;
    }

    return value != before;
}

bool skin_stepper(UiContext& ctx, const UiRect& r, float& value, float min_value, float max_value,
                  float step, const UiNumberFormat& fmt, int font, bool enabled, UiId id,
                  bool integral, bool hot)
{
    const int h = std::min(r.h, skin_px(ctx, skin::widget_h));
    const auto [down, up, well] = cycler_parts(ctx, UiRect{r.x, r.y + (r.h - h) / 2, r.w, h});

    const UiId up_id = sub_id(id, stepper_up_salt);
    const UiId down_id = sub_id(id, stepper_down_salt);
    const bool hu = enabled && ui_hover(ctx, up, id);
    const bool hd = enabled && ui_hover(ctx, down, id);
    if (hu || hd) {
        ctx.note_hovered(id);
    }
    if (enabled) {
        ctx.register_focusable(id, UiFocusKind::Adjustable);
    }

    const float before = value;
    int delta = 0;
    bool activated = false;

    if (!ctx.draw && enabled) {
        activated = ctx.consume_activation(id);
        if (ctx.click && (hu || hd)) {
            ctx.capture(hu ? up_id : down_id);
        }
        if (const int adjust = ctx.consume_adjust(id); adjust != 0) {
            delta = adjust;
        }
        else if (hu && ctx.captured_id() == up_id) {
            if (ctx.repeat_tick(up_id, 1)) {
                delta = 1;
            }
        }
        else if (hd && ctx.captured_id() == down_id) {
            if (ctx.repeat_tick(down_id, -1)) {
                delta = -1;
            }
        }
        else {
            ctx.clear_repeat(up_id);
            ctx.clear_repeat(down_id);
        }
        if ((delta > 0 && value >= max_value) || (delta < 0 && value <= min_value)) {
            delta = 0;
        }
        if (delta != 0) {
            const float unit = step > 0.0f ? step : 1.0f;
            // A held arrow on a long range speeds up to 10 steps at a time, landing on multiples of
            // the big step so the values it passes stay round.
            constexpr int accelerate_after_repeats = 12;
            constexpr float accelerated_steps = 10.0f;
            const float big = unit * accelerated_steps;
            if (ctx.lmb_down && ctx.repeat_count() >= accelerate_after_repeats
                && max_value - min_value >= big * accelerated_steps) {
                const float slots = value / big;
                value = (delta > 0 ? std::floor(slots + 1.0e-4f) + 1.0f : std::ceil(slots - 1.0e-4f) - 1.0f) * big;
            }
            else {
                value += static_cast<float>(delta) * unit;
            }
            value = std::clamp(value, min_value, max_value);
        }
    }

    const UiId edit_id = sub_id(id, stepper_edit_salt);
    const bool editing = text_focused(ctx, edit_id);
    std::string shown = ui_format_number(value, fmt);
    if (!ctx.draw) {
        numeric_entry(ctx, well, edit_id, value, fmt, integral, min_value, max_value, font, enabled, activated);
        return value != before;
    }

    const UiRect dw = ui_draw_rect(ctx, well);
    if (editing) {
        skin_bevel_field(ctx, dw.x, dw.y, dw.w, dw.h);
    }
    else {
        skin_slot(ctx, dw.x, dw.y, dw.w, dw.h);
    }
    const bool active = focus_shown(ctx, id);
    draw_cycler_arrows(ctx, ui_draw_rect(ctx, down), ui_draw_rect(ctx, up), enabled, hd || active, hu || active);
    rf::gr::set_color(editing ? skin::txt_hi : value_color(enabled, hot || hu || hd));
    const std::string text = editing ? ctx.text_buffer() + "_" : shown;
    // Large values (connection rate, max FPS) would run out of the well; shrink them to fit.
    const int text_pad = skin_px(ctx, 10.0f);
    const SkinText fitted = skin_fit_text(ctx, text, font, std::max(0, dw.w - 2 * text_pad));
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, dw.x + dw.w / 2,
                           dw.y + (dw.h - font_h(fitted.font)) / 2, fitted.text.c_str(), fitted.font);
    return false;
}

bool skin_swatch(UiContext& ctx, const UiRect& r, std::optional<uint32_t>& rgba, int font, bool enabled,
                 UiId id, bool hot)
{
    const int sw = skin_px(ctx, skin::swatch_size);
    const int gap = skin_px(ctx, 12.0f);
    // The box takes the rest of `r`, but always holds RRGGBBAA plus the edit caret.
    const int hw = std::max(r.w - sw - gap,
                            rf::gr::get_string_size("DDDDDDDD_", font).first + 2 * skin_px(ctx, 8.0f));
    const int hh = skin_px(ctx, skin::readout_h);
    const int x0 = r.x + r.w - (sw + gap + hw);
    const UiRect swatch{x0, r.y + (r.h - sw) / 2, sw, sw};
    const UiRect box{x0 + sw + gap, r.y + (r.h - hh) / 2, hw, hh};

    const UiId box_id = sub_id(id, swatch_edit_salt);
    const bool editing = text_focused(ctx, box_id);
    const bool swatch_hot = enabled && ui_hover(ctx, swatch, id);
    std::string shown = rgba ? format_hex_color_string(*rgba) : std::string{};

    if (!ctx.draw) {
        if (swatch_hot) {
            ctx.note_hovered(id);
        }
        if (enabled) {
            ctx.register_focusable(id, UiFocusKind::Activate);
        }
        const UiTextInputResult res = ui_text_input(ctx, box, box_id,
            editing ? ctx.text_buffer() : shown, 8, ui_char_filter_hex, "RRGGBB", font, enabled);
        if (res.gained_focus) {
            ctx.text_buffer() = shown;
            std::optional<uint32_t>* const target = &rgba;
            ctx.set_text_commit_callback([c = &ctx, target](bool commit) {
                if (!commit) {
                    return;
                }
                if (const std::optional<uint32_t> parsed = parse_hex_color_string(c->text_buffer())) {
                    *target = parsed;
                }
            });
            ui_play_click();
        }
        return (enabled && ctx.consume_activation(id)) || (swatch_hot && ctx.click);
    }

    const UiRect ds = ui_draw_rect(ctx, swatch);
    skin_slot(ctx, ds.x, ds.y, ds.w, ds.h);
    const int inset = skin_px(ctx, 5.0f);
    const UiRect chip{ds.x + inset, ds.y + inset, ds.w - 2 * inset, ds.h - 2 * inset};
    if (rgba) {
        fill(chip.x, chip.y, chip.w, chip.h, rf::Color::from_hex(*rgba, true));
    }
    else {
        fill(chip.x, chip.y, chip.w, chip.h, rf::Color{0x30, 0x30, 0x34, 255});
        ui_aa_line(static_cast<float>(chip.x + 1), static_cast<float>(chip.y + chip.h - 1),
                   static_cast<float>(chip.x + chip.w - 1), static_cast<float>(chip.y + 1),
                   2.0f * ctx.scale(), skin::txt);
    }
    if (swatch_hot) {
        frame(ds.x, ds.y, ds.w, ds.h, skin_px(ctx, 2.0f), skin::txt_hi);
    }

    const UiRect db = ui_draw_rect(ctx, box);
    if (editing) {
        skin_bevel_field(ctx, db.x, db.y, db.w, db.h);
    }
    else {
        skin_slot(ctx, db.x, db.y, db.w, db.h);
    }
    // An unset optional colour uses the game's own colour. The placeholder says so, dimmed so it
    // does not read as a stored value.
    rf::gr::set_color(editing ? skin::txt_hi : (rgba ? value_color(enabled, hot) : skin::txt_hint));
    const std::string text = editing ? ctx.text_buffer() + "_" : (rgba ? shown : std::string{"Default"});
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, db.x + db.w / 2, db.y + (db.h - font_h(font)) / 2,
                           text.c_str(), font);
    return false;
}

// ---------------------------------------------------------------------------
// Colour picker
// ---------------------------------------------------------------------------

SkinHsva skin_rgba_to_hsva(uint32_t rgba)
{
    const float r = static_cast<float>((rgba >> 24) & 0xFF) / 255.0f;
    const float g = static_cast<float>((rgba >> 16) & 0xFF) / 255.0f;
    const float b = static_cast<float>((rgba >> 8) & 0xFF) / 255.0f;
    const float a = static_cast<float>(rgba & 0xFF) / 255.0f;
    const float hi = std::max({r, g, b});
    const float d = hi - std::min({r, g, b});
    float h = 0.0f;
    if (d > 0.0f) {
        if (hi == r) {
            h = (g - b) / d;
        }
        else if (hi == g) {
            h = 2.0f + (b - r) / d;
        }
        else {
            h = 4.0f + (r - g) / d;
        }
        h /= 6.0f;
        if (h < 0.0f) {
            h += 1.0f;
        }
    }
    return SkinHsva{h, hi > 0.0f ? d / hi : 0.0f, hi, a};
}

uint32_t skin_hsva_to_rgba(const SkinHsva& c)
{
    const float h6 = (c.h - std::floor(c.h)) * 6.0f;
    const float f = h6 - std::floor(h6);
    const float p = c.v * (1.0f - c.s);
    const float q = c.v * (1.0f - c.s * f);
    const float t = c.v * (1.0f - c.s * (1.0f - f));
    const float sectors[6][3] = {{c.v, t, p}, {q, c.v, p}, {p, c.v, t}, {p, q, c.v}, {t, p, c.v}, {c.v, p, q}};
    const float* rgb = sectors[static_cast<int>(h6) % 6];
    const auto byte = [](float x) {
        return static_cast<uint32_t>(std::lround(std::clamp(x, 0.0f, 1.0f) * 255.0f));
    };
    return (byte(rgb[0]) << 24) | (byte(rgb[1]) << 16) | (byte(rgb[2]) << 8) | byte(c.a);
}

bool skin_sv_square(UiContext& ctx, const UiRect& r, SkinHsva& c, UiId id)
{
    if (!ctx.draw) {
        ctx.register_focusable(id, UiFocusKind::Adjustable);
        float tx = 0.0f;
        float ty = 0.0f;
        if (picker_drag(ctx, r, id, tx, ty)) {
            c.s = tx;
            c.v = 1.0f - ty;
            return true;
        }
        return picker_adjust(ctx, id, c.s, picker_key_step);
    }

    const UiRect d = ui_draw_rect(ctx, r);
    picker_well(ctx, d);
    // The flat hue under a white ramp that fades out to the right and a black ramp that fades in
    // downwards, which is the HSV mix.
    fill(d.x, d.y, d.w, d.h, rf::Color::from_hex(skin_hsva_to_rgba(SkinHsva{c.h, 1.0f, 1.0f, 1.0f}), true));
    stretch_bitmap(white_fade_right_bitmap(), d, rf::Color{255, 255, 255, 255}, false);
    stretch_bitmap(white_fade_down_bitmap(), d, rf::Color{0, 0, 0, 255}, false);

    // A ring the cursor sits inside: black then white, so it reads on light and dark colours.
    const int ring = skin_px(ctx, 14.0f);
    const int mx = d.x + static_cast<int>(c.s * static_cast<float>(d.w - 1)) - ring / 2;
    const int my = d.y + static_cast<int>((1.0f - c.v) * static_cast<float>(d.h - 1)) - ring / 2;
    frame(mx, my, ring, ring, skin_px(ctx, 1.0f), rf::Color{0, 0, 0, 255});
    frame(mx + skin_px(ctx, 1.0f), my + skin_px(ctx, 1.0f), ring - 2 * skin_px(ctx, 1.0f),
          ring - 2 * skin_px(ctx, 1.0f), skin_px(ctx, 2.0f), skin::txt_hi);
    draw_focus_if_focused(ctx, d, id);
    return false;
}

bool skin_hue_strip(UiContext& ctx, const UiRect& r, SkinHsva& c, UiId id)
{
    if (!ctx.draw) {
        ctx.register_focusable(id, UiFocusKind::Adjustable);
        float tx = 0.0f;
        float ty = 0.0f;
        if (picker_drag(ctx, r, id, tx, ty)) {
            c.h = ty;
            return true;
        }
        return picker_adjust(ctx, id, c.h, picker_key_step);
    }

    const UiRect d = ui_draw_rect(ctx, r);
    picker_well(ctx, d);
    stretch_bitmap(hue_bitmap(), d, rf::Color{255, 255, 255, 255}, false);
    strip_marker(ctx, d, c.h);
    draw_focus_if_focused(ctx, d, id);
    return false;
}

bool skin_alpha_strip(UiContext& ctx, const UiRect& r, SkinHsva& c, UiId id)
{
    if (!ctx.draw) {
        ctx.register_focusable(id, UiFocusKind::Adjustable);
        float tx = 0.0f;
        float ty = 0.0f;
        if (picker_drag(ctx, r, id, tx, ty)) {
            c.a = 1.0f - ty;
            return true;
        }
        // Negated so a key moves the marker the same way as on the hue strip.
        return picker_adjust(ctx, id, c.a, -picker_key_step);
    }

    const UiRect d = ui_draw_rect(ctx, r);
    picker_well(ctx, d);
    checker(ctx, d);
    // Opaque at the top, like the value axis of the square beside it.
    const rf::Color solid = rf::Color::from_hex(skin_hsva_to_rgba(SkinHsva{c.h, c.s, c.v, 1.0f}), true);
    stretch_bitmap(white_fade_down_bitmap(), d, solid, true);
    strip_marker(ctx, d, 1.0f - c.a);
    draw_focus_if_focused(ctx, d, id);
    return false;
}

void skin_color_preview(const UiContext& ctx, const UiRect& r, std::optional<uint32_t> rgba, int font)
{
    const UiRect d = ui_draw_rect(ctx, r);
    if (rgba) {
        checker(ctx, d);
        fill(d.x, d.y, d.w, d.h, rf::Color::from_hex(*rgba, true));
    }
    else {
        fill(d.x, d.y, d.w, d.h, skin::field);
        const SkinText fitted = skin_fit_text(ctx, "Default", font, d.w - 2 * skin_px(ctx, 6.0f));
        rf::gr::set_color(skin::txt_hint);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, d.x + d.w / 2, d.y + (d.h - font_h(fitted.font)) / 2,
                               fitted.text.c_str(), fitted.font);
    }
    frame(d.x, d.y, d.w, d.h, skin_px(ctx, 1.0f), rgba ? rf::Color{0, 0, 0, 255} : skin::border_dim);
}

bool skin_color_chip(UiContext& ctx, const UiRect& r, std::optional<uint32_t> rgba, int font, UiId id)
{
    const bool hovered = ui_hover(ctx, r, id);
    if (!ctx.draw) {
        if (hovered) {
            ctx.note_hovered(id);
        }
        ctx.register_focusable(id, UiFocusKind::Activate);
        return ctx.consume_activation(id) || (hovered && ctx.click);
    }
    skin_color_preview(ctx, r, rgba, font);
    const UiRect d = ui_draw_rect(ctx, r);
    if (hovered) {
        frame(d.x, d.y, d.w, d.h, skin_px(ctx, 2.0f), skin::txt_hi);
    }
    draw_focus_if_focused(ctx, d, id);
    return false;
}

bool skin_field(UiContext& ctx, const UiRect& r, UiId id, std::string& text, size_t max_len,
                UiCharFilter filter, const char* hint, int font, bool enabled, bool invalid,
                const char* warn_text)
{
    const bool editing = text_focused(ctx, id);

    if (!ctx.draw) {
        if (enabled) {
            ctx.register_focusable(id, UiFocusKind::Text);
        }
        const UiTextInputResult res =
            ui_text_input(ctx, r, id, text, max_len, filter, hint, font, enabled);
        if (res.gained_focus) {
            ui_play_click();
        }
        return res.changed;
    }

    const UiRect d = ui_draw_rect(ctx, r);
    skin_bevel_field(ctx, d.x, d.y, d.w, d.h);
    if (invalid) {
        frame(d.x, d.y, d.w, d.h, skin_px(ctx, 2.0f), skin::lamp_red);
        if (warn_text && *warn_text) {
            // Fields sit at the right edge of their row, so the message goes on the label side.
            rf::gr::set_color(skin::warn);
            rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, d.x - skin_px(ctx, 12.0f),
                                   d.y + (d.h - font_h(font)) / 2, warn_text, font);
        }
    }
    const int pad = skin_px(ctx, 10.0f);
    const int ty = d.y + (d.h - font_h(font)) / 2;
    if (text.empty() && !editing) {
        rf::gr::set_color(skin::txt_hint);
        rf::gr::string(d.x + pad, ty, hint ? hint : "", font);
    }
    else {
        rf::gr::set_color(editing ? skin::txt_hi : value_color(enabled, false));
        const std::string shown = hud_fit_string(text, d.w - 2 * pad, nullptr, font);
        rf::gr::string(d.x + pad, ty, shown.c_str(), font);
        if (editing) {
            const int tw = rf::gr::get_string_size(shown, font).first;
            fill(d.x + pad + tw + skin_px(ctx, 2.0f), ty, skin_px(ctx, 2.0f), font_h(font), skin::txt_hi);
        }
    }
    if (!editing) {
        draw_focus_if_focused(ctx, d, id);
    }
    return false;
}

bool skin_search_box(UiContext& ctx, const UiRect& r, UiId id, std::string& text, int font)
{
    const int icon = skin_px(ctx, 20.0f);
    const int pad = skin_px(ctx, 12.0f);
    const UiRect inner{r.x + pad + icon + skin_px(ctx, 10.0f), r.y,
                       std::max(0, r.w - 2 * pad - 2 * icon - skin_px(ctx, 20.0f)), r.h};
    const UiRect clear{r.x + r.w - pad - icon, r.y + (r.h - icon) / 2, icon, icon};

    bool changed = false;
    const bool editing = text_focused(ctx, id);

    if (!ctx.draw) {
        ctx.register_focusable(id, UiFocusKind::Text);
        if (!text.empty() && ctx.click && ui_hover(ctx, r, id) && ui_point_in(ctx.mx, ctx.my, clear)) {
            if (editing) {
                ctx.release_text_focus(false);
            }
            text.clear();
            ui_play_click();
            return true;
        }
        const UiTextInputResult res = ui_text_input(ctx, r, id, text, 40, ui_char_filter_filename,
                                                    "Search options...", font, true);
        if (res.gained_focus) {
            ui_play_click();
        }
        changed = res.changed;
        return changed;
    }

    const UiRect d = ui_draw_rect(ctx, r);
    const UiRect di = ui_draw_rect(ctx, inner);
    skin_bevel_field(ctx, d.x, d.y, d.w, d.h);
    skin_icon("af_search_icon", d.x + pad, d.y + (d.h - icon) / 2, icon,
              editing || !text.empty() ? skin::txt : skin::txt_dim);
    if (text.empty() && !editing) {
        const SkinText hint = skin_fit_text(ctx, "Search options...", font, di.w);
        rf::gr::set_color(skin::txt_hint);
        rf::gr::string(di.x, d.y + (d.h - font_h(hint.font)) / 2, hint.text.c_str(), hint.font);
    }
    else {
        rf::gr::set_color(skin::txt_hi);
        const SkinText shown = skin_fit_text(ctx, text, font, di.w);
        const int ty = d.y + (d.h - font_h(shown.font)) / 2;
        rf::gr::string(di.x, ty, shown.text.c_str(), shown.font);
        if (editing) {
            const int tw = rf::gr::get_string_size(shown.text, shown.font).first;
            fill(di.x + tw + skin_px(ctx, 2.0f), ty, skin_px(ctx, 2.0f), font_h(shown.font), skin::txt_hi);
        }
        if (!text.empty()) {
            const UiRect dc = ui_draw_rect(ctx, clear);
            skin_icon("af_close_icon", dc.x, dc.y, icon, skin::txt_hint);
        }
    }
    if (!editing) {
        draw_focus_if_focused(ctx, d, id);
    }
    return false;
}

SkinKeyCapResult skin_key_cap(UiContext& ctx, const UiRect& r, const char* text, bool bound,
                              bool capturing, bool enabled, UiId id, int font, bool inherited)
{
    SkinKeyCapResult out;
    const int clear_size = skin_px(ctx, 14.0f);
    const int pad = skin_px(ctx, 10.0f);
    const UiRect clear{r.x + r.w - pad - clear_size, r.y + (r.h - clear_size) / 2, clear_size,
                       clear_size};

    const bool hovered = enabled && ui_hover(ctx, r, id);
    const bool over_clear = hovered && ui_point_in(ctx.mx, ctx.my, clear);
    if (hovered) {
        ctx.note_hovered(id);
    }
    if (enabled) {
        ctx.register_focusable(id, UiFocusKind::Activate);
    }
    // The x is offered on hover or focus, and only when there is something to unbind.
    const bool show_clear = enabled && bound && !capturing && (hovered || focus_shown(ctx, id));

    if (!ctx.draw) {
        if (!enabled || capturing) {
            return out;
        }
        if (ctx.consume_activation(id)) {
            out.activated = true;
        }
        else if (ctx.click && hovered) {
            if (show_clear && over_clear) {
                out.cleared = true;
            }
            else {
                out.activated = true;
            }
        }
        else if (ctx.rclick && hovered && bound) {
            out.cleared = true;
        }
        return out;
    }

    const UiRect d = ui_draw_rect(ctx, r);
    skin_slot(ctx, d.x, d.y, d.w, d.h);
    if (capturing) {
        frame(d.x, d.y, d.w, d.h, skin_px(ctx, 2.0f), skin::lamp_red);
    }

    int tx = d.x + pad;
    if (capturing) {
        skin_led(ctx, tx + skin_px(ctx, 4.0f), d.y + d.h / 2, 7.0f, 20.0f, true);
        tx += skin_px(ctx, 18.0f);
    }
    const bool lit = hovered || focus_shown(ctx, id);
    rf::Color color = skin::txt_dim;
    if (capturing) {
        color = skin::warn;
    }
    else if (enabled && inherited) {
        color = lit ? skin::txt : skin::txt_hint;
    }
    else if (enabled && bound) {
        color = lit ? skin::txt_hi : skin::txt;
    }
    rf::gr::set_color(color);
    const int text_w = std::max(0, d.w - (tx - d.x) - pad - (show_clear ? clear_size + pad : 0));
    const SkinText cap = skin_fit_text(ctx, text ? text : "", font, text_w);
    rf::gr::string(tx, d.y + (d.h - font_h(cap.font)) / 2, cap.text.c_str(), cap.font);

    if (show_clear) {
        const UiRect dc = ui_draw_rect(ctx, clear);
        skin_icon("af_close_icon", dc.x, dc.y, clear_size, over_clear ? skin::txt_hi : skin::txt);
    }
    draw_focus_if_focused(ctx, d, id);
    return out;
}

bool skin_reorder_row(UiContext& ctx, const UiRect& r, const char* label, bool selected, int font,
                      UiId id)
{
    const bool hovered = ui_hover(ctx, r, id);
    if (hovered) {
        ctx.note_hovered(id);
    }
    ctx.register_focusable(id, UiFocusKind::Activate);

    if (!ctx.draw) {
        if (ctx.consume_activation(id)) {
            return true;
        }
        return hovered && ctx.click;
    }

    const UiRect d = ui_draw_rect(ctx, r);
    if (selected) {
        skin_selection_bar(ctx, d.x, d.y, d.w, d.h);
    }

    // 3x2 grip dots, clear of the selection LED.
    const int dot = std::max(1, skin_px(ctx, 3.0f));
    const int step = skin_px(ctx, 6.0f);
    const int gx = d.x + skin_px(ctx, 26.0f);
    const int gy = d.y + d.h / 2 - step;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 2; ++col) {
            fill(gx + col * step, gy + row * step, dot, dot, skin::txt_dim);
        }
    }

    rf::gr::set_color(selected || hovered ? skin::txt_hi : skin::txt);
    const int tx = gx + 2 * step + skin_px(ctx, 10.0f);
    const SkinText fitted = skin_fit_text(ctx, label ? label : "", font,
                                          std::max(0, d.x + d.w - tx - skin_px(ctx, 10.0f)));
    rf::gr::string(tx, d.y + (d.h - font_h(fitted.font)) / 2, fitted.text.c_str(), fitted.font);
    draw_focus_if_focused(ctx, d, id);
    return false;
}

int skin_picker_caption_h(const UiContext& ctx, int font)
{
    return font_h(font) + skin_px(ctx, 6.0f);
}

bool skin_picker_tile(UiContext& ctx, const UiRect& r, int bitmap, int index, bool selected,
                      const char* caption, bool caption_line, int font, UiId id)
{
    const bool hovered = ui_hover(ctx, r, id);
    if (hovered) {
        ctx.note_hovered(id);
    }
    ctx.register_focusable(id, UiFocusKind::Activate);

    if (!ctx.draw) {
        if (ctx.consume_activation(id)) {
            return true;
        }
        return hovered && ctx.click;
    }

    const UiRect d = ui_draw_rect(ctx, r);
    const bool lit = hovered || focus_shown(ctx, id);
    const int cap_h = caption_line ? skin_picker_caption_h(ctx, font) : 0;
    const int img_h = std::max(1, d.h - cap_h);
    fill(d.x, d.y, d.w, d.h, skin::field);
    if (bitmap >= 0) {
        int bw = 0;
        int bh = 0;
        rf::bm::get_dimensions(bitmap, &bw, &bh);
        // Portraits are not square, so the blit is letterboxed inside the image area.
        int dw = d.w;
        int dh = img_h;
        if (bw > 0 && bh > 0) {
            const float fit = std::min(static_cast<float>(d.w) / static_cast<float>(bw),
                                       static_cast<float>(img_h) / static_cast<float>(bh));
            dw = std::max(1, static_cast<int>(static_cast<float>(bw) * fit));
            dh = std::max(1, static_cast<int>(static_cast<float>(bh) * fit));
        }
        rf::gr::set_color(255, 255, 255, 255);
        rf::gr::bitmap_scaled(bitmap, d.x + (d.w - dw) / 2, d.y + (img_h - dh) / 2, dw, dh, 0, 0, bw,
                              bh, false, false, rf::gr::bitmap_clamp_mode);
    }
    else if (caption && !caption_line) {
        rf::gr::set_color(skin::txt_hint);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, d.x + d.w / 2, d.y + (img_h - font_h(font)) / 2,
                               ui_fit_middle(caption, d.w - skin_px(ctx, 8.0f), font).c_str(), font);
    }

    if (caption_line) {
        rf::gr::set_color(selected || lit ? skin::txt_hi : skin::txt);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, d.x + d.w / 2,
                               d.y + img_h + (cap_h - font_h(font)) / 2,
                               ui_fit_middle(caption ? caption : "", d.w - skin_px(ctx, 8.0f), font)
                                   .c_str(),
                               font);
    }

    if (selected) {
        frame(d.x, d.y, d.w, d.h, skin_px(ctx, 4.0f), glow_hi);
        frame(d.x + skin_px(ctx, 1.0f), d.y + skin_px(ctx, 1.0f), d.w - 2 * skin_px(ctx, 1.0f),
              d.h - 2 * skin_px(ctx, 1.0f), skin_px(ctx, 2.0f), skin::lamp_red);
    }
    else if (lit) {
        frame(d.x, d.y, d.w, d.h, skin_px(ctx, 2.0f), skin::txt_hi);
    }
    else {
        frame(d.x, d.y, d.w, d.h, skin_px(ctx, 1.0f), skin::field_edge);
    }
    if (selected && lit) {
        // The selection ring stays red, so the highlight moves inside it.
        const int in = skin_px(ctx, 4.0f);
        frame(d.x + in, d.y + in, d.w - 2 * in, d.h - 2 * in, skin_px(ctx, 1.0f), skin::txt_hi);
    }

    const std::string tag = std::to_string(index);
    const int tag_w = rf::gr::get_string_size(tag, font).first + skin_px(ctx, 8.0f);
    const int tag_h = font_h(font) + skin_px(ctx, 2.0f);
    fill(d.x + d.w - tag_w - skin_px(ctx, 5.0f), d.y + img_h - tag_h - skin_px(ctx, 5.0f), tag_w,
         tag_h, rf::Color{0, 0, 0, 179});
    rf::gr::set_color(selected ? skin::txt_hi : skin::txt);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER,
                           d.x + d.w - skin_px(ctx, 5.0f) - tag_w / 2,
                           d.y + img_h - tag_h - skin_px(ctx, 4.0f), tag.c_str(), font);
    return false;
}

int skin_badge_width(const UiContext& ctx, const char* text, int font, bool has_marker)
{
    const int pad = skin_px(ctx, 8.0f);
    const int marker = has_marker ? skin_px(ctx, 19.0f) : 0;
    return rf::gr::get_string_size(text, font).first + 2 * pad + marker;
}

void skin_badge(const UiContext& ctx, const UiRect& r, const char* text, rf::Color color, int font,
                const char* icon, bool lamp)
{
    if (!ctx.draw) {
        return;
    }
    const UiRect d = ui_draw_rect(ctx, r);
    const float radius = 4.0f * ctx.scale();
    ui_rounded_rect(static_cast<float>(d.x), static_cast<float>(d.y), static_cast<float>(d.w),
                    static_cast<float>(d.h), radius, rf::Color{0x10, 0x10, 0x14, 235});
    ui_rounded_rect_border(static_cast<float>(d.x), static_cast<float>(d.y),
                           static_cast<float>(d.w), static_cast<float>(d.h), radius,
                           static_cast<float>(skin_px(ctx, 1.0f)), color);
    int tx = d.x + skin_px(ctx, 8.0f);
    if (lamp) {
        lamp_dot(ctx, tx + skin_px(ctx, 4.0f), d.y + d.h / 2, 7.0f, color, true);
        tx += skin_px(ctx, 13.0f);
    }
    else if (icon) {
        const int size = skin_px(ctx, 13.0f);
        skin_icon(icon, tx, d.y + (d.h - size) / 2, size, color);
        tx += size + skin_px(ctx, 6.0f);
    }
    rf::gr::set_color(color);
    rf::gr::string(tx, d.y + (d.h - font_h(font)) / 2, text, font);
}

void skin_scrollbar(const UiContext& ctx, const UiRect& view, int content_h, float scroll,
                    bool dragging)
{
    if (content_h <= view.h) {
        return;
    }
    const UiScrollbarGeometry bar = ui_scrollbar_geometry(ctx, view, content_h, scroll);
    const UiRect& g = bar.gutter;
    skin_slot(ctx, g.x, g.y, g.w, g.h);
    const int cap = bar.track.y - g.y;
    const int edge = skin_px(ctx, 1.0f);
    const int arrow = cap - 2 * edge;
    if (cap > 0) {
        for (const bool top : {true, false}) {
            const int by = top ? g.y : bar.track.y + bar.track.h;
            fill(g.x, by, g.w, cap, skin::scroll_box);
            frame(g.x, by, g.w, cap, edge, skin::scroll_box_edge);
            skin_arrow(top ? SkinArrow::UpOutline : SkinArrow::DownOutline, g.x + (g.w - arrow) / 2, by + edge,
                       arrow, skin::scroll_arrow);
        }
    }

    const int inset = skin_px(ctx, 3.0f);
    const int thumb_w = std::max(2, g.w - 2 * inset);
    skin_steel_plate(ctx, g.x + inset, bar.thumb_y, thumb_w, bar.thumb_h);
    if (dragging) {
        const int core = std::max(2, skin_px(ctx, 4.0f));
        fill(g.x + inset + (thumb_w - core) / 2, bar.thumb_y + skin_px(ctx, 4.0f), core,
             std::max(0, bar.thumb_h - 2 * skin_px(ctx, 4.0f)), skin::teal_led);
    }
}
