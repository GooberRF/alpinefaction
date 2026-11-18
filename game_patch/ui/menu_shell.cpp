#include <algorithm>
#include <cctype>
#include <cmath>
#include <string_view>
#include <common/utils/string-utils.h>
#include "menu_shell.h"
#include "reference_data.h"
#include "settings_skin.h"
#include "../graphics/gr.h"
#include "../graphics/ui_draw.h"
#include "../hud/hud_internal.h"
#include "../misc/misc.h"
#include "../misc/settings_registry.h"
#include "../multi/map_thumbnails.h"
#include "../rf/bmpman.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/gameseq.h"
#include "../rf/input.h"
#include "../rf/level.h"
#include "../rf/localize.h"
#include "../rf/multi.h"
#include "../rf/os/frametime.h"
#include "../rf/player/player.h"
#include "../rf/ui.h"

namespace
{

// The panel frame around the content area: the left channel and the top and bottom ridges.
constexpr float frame_left = 34.0f;
constexpr float frame_edge = 22.0f;
// The frame's steel margin, and its darker channel inside it.
constexpr float frame_margin = 6.0f;
constexpr float frame_channel_w = 20.0f;
// Space kept between anything in the content area and the frame's right edge.
constexpr float content_clearance = 16.0f;

// The bar down the left edge is a panel frame's left edge, at the panels' scale so it never stretches with the screen.
// The buttons tuck under it, and every one shows the same width of its plate between the bar and its face.
constexpr float bar_w = frame_left;
constexpr float bar_tuck = 14.0f;
constexpr float plate_shown_w = 14.0f;
constexpr float bar_screw_size = 14.0f;
// The face of a column button; the end cap follows it.
constexpr float well_visible_w = 260.0f;
// The panels stop short of the right edge, and short of where a very wide screen would make the lists unreadable.
constexpr float right_gutter = 24.0f;
constexpr float panel_max_w = 2400.0f;
// The title bar, a gap under the path strip, and its text after its block.
constexpr float title_bar_y = menu_strip_h + 20.0f;
constexpr float title_bar_h = 84.0f;
constexpr float title_inset = title_bar_h + 24.0f;
constexpr float title_font_px = 46.0f;
constexpr float path_font_px = 20.0f;
// A step's chevron: where it starts after the step, and where the next step does.
constexpr float chevron_gap = 16.0f;
constexpr float chevron_step = 34.0f;
constexpr float chevron_arm = 5.0f;
// The strips' lettering, in from the bar and the right edge.
constexpr float strip_pad = 20.0f;
// The player's portrait, inset in the path strip, the name before it up to its widest, and the path kept clear of
// both.
constexpr float portrait_inset = 4.0f;
constexpr float player_name_gap = 12.0f;
constexpr float player_name_max_w = 320.0f;
constexpr float path_player_gap = 40.0f;
constexpr float context_font_px = 18.0f;
// A hint's key cap, and the gaps after it and after its label.
constexpr float hint_key_font_px = 14.0f;
constexpr float hint_label_font_px = 17.0f;
constexpr float hint_cap_h = 26.0f;
constexpr float hint_cap_pad = 7.0f;
constexpr float hint_cap_radius = 4.0f;
constexpr float hint_arrows_gap = 6.0f;
constexpr float hint_label_gap = 8.0f;
constexpr float hint_gap = 24.0f;

constexpr rf::Color modal_dim{0, 0, 0, 191};

constexpr float slide_seconds = 0.25f;
constexpr float min_animation_step = 0.002f;

// The path strip menu_draw_frame drew last, and in which frame.
struct DrawnStrip
{
    int frame = 0;
    int h = 0;
};
DrawnStrip g_drawn_strip;

// A menu state's crumb. None for a state with no menu screen of its own: gameplay, the server list, which is the
// Multi menu with its JOIN panel open, or the message log, a dialog over the game that no jump could end at.
std::string state_crumb(rf::GameState state)
{
    switch (state) {
        case rf::GS_MAIN_MENU:
            return menu_root_crumb();
        case rf::GS_EXTRAS_MENU:
            return menu_stock_label(rf::strings::extras);
        case rf::GS_SAVE_GAME_MENU:
            return menu_stock_label(rf::strings::save_game);
        case rf::GS_LOAD_GAME_MENU:
            return menu_stock_label(rf::strings::load_game);
        case rf::GS_OPTIONS_MENU:
            return menu_stock_label(rf::strings::options);
        case rf::GS_MULTI_MENU:
            return menu_stock_label(rf::strings::multiplayer);
        case rf::GS_MULTI_CREATE_GAME:
            return "HOST GAME";
        default:
            return {};
    }
}

// The hint bar's right end: the game running beneath, else Alpine's version as the legacy main menu shows it.
std::string strip_context()
{
    if (gameseq_is_stacked(rf::GS_GAMEPLAY) || gameseq_is_stacked(rf::GS_MULTI_LIMBO)
        || gameseq_is_stacked(rf::GS_MULTI_LEVEL_DOWNLOAD) || gameseq_is_stacked(rf::GS_MULTI_LIMBO_JUST_JOINED)) {
        if (rf::is_multi) {
            return rf::netgame.name.c_str();
        }
        return (rf::level.name.empty() ? rf::level.filename : rf::level.name).c_str();
    }
    return main_menu_version_text();
}

// The hint bar's lettering, against the whole screen: the context at the right end in up to half the bar, and the
// hints in what is left from `hints_x` to `hints_r`.
struct StripLayout
{
    int mid = 0;
    int hints_x = 0;
    int hints_r = 0;
    SkinText context{};
    UiRect context_rect;
};

StripLayout strip_layout(const UiContext& ctx)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const int strip_h = s(menu_strip_h);
    const int right = rf::gr::clip_width() - s(strip_pad);
    StripLayout strip;
    strip.mid = rf::gr::clip_height() - strip_h + strip_h / 2;
    strip.hints_x = s(bar_w) + s(strip_pad);
    strip.hints_r = right;
    const std::string context = strip_context();
    const int context_font = menu_font(false, context_font_px, ctx.scale());
    const int half = (right - strip.hints_x) / 2;
    const int context_room = context.empty() ? 0 : std::min(rf::gr::get_string_size(context, context_font).first, half);
    if (context_room <= 0) {
        return strip;
    }
    strip.context = skin_fit_text(ctx, context, context_font, context_room);
    const int w = rf::gr::get_string_size(strip.context.text, strip.context.font).first;
    const int h = rf::gr::get_font_height(strip.context.font);
    strip.context_rect = UiRect{right - w, strip.mid - h / 2, w, h};
    strip.hints_r = right - context_room - s(40.0f);
    return strip;
}

// Drawn, as the menu fonts may lack arrows: pointing along (`dx`, `dy`) to (`x`, `y`), its arms `arm` back and as far
// to either side.
void draw_chevron(float x, float y, float dx, float dy, float arm, float line_w, rf::Color color)
{
    const float back_x = x - dx * arm;
    const float back_y = y - dy * arm;
    ui_aa_line(back_x - dy * arm, back_y + dx * arm, x, y, line_w, color);
    ui_aa_line(x, y, back_x + dy * arm, back_y - dx * arm, line_w, color);
}

float hint_arrows_w(MenuHintArrows arrows, float arm, float gap)
{
    return (arrows == MenuHintArrows::UpDown ? 4.0f : 2.0f) * arm + gap;
}

void draw_hint_arrows(MenuHintArrows arrows, float cx, float cy, float arm, float gap, float line_w)
{
    const float half = hint_arrows_w(arrows, arm, gap) / 2.0f;
    if (arrows == MenuHintArrows::UpDown) {
        draw_chevron(cx - half + arm, cy - arm / 2.0f, 0.0f, -1.0f, arm, line_w, skin::txt_hi);
        draw_chevron(cx + half - arm, cy + arm / 2.0f, 0.0f, 1.0f, arm, line_w, skin::txt_hi);
    }
    else {
        draw_chevron(cx - half, cy, -1.0f, 0.0f, arm, line_w, skin::txt_hi);
        draw_chevron(cx + half, cy, 1.0f, 0.0f, arm, line_w, skin::txt_hi);
    }
}

bool is_esc_hint(const MenuHint& hint)
{
    return hint.key == "ESC";
}

// From `x` on the bar's middle `mid`, the key a cap with what it does after it. Returns its width, and draws it
// only with `draw`.
int build_hint(const UiContext& ctx, const MenuHint& hint, int x, int mid, bool draw)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const float scale = ctx.scale();
    const int key_font = menu_font(true, hint_key_font_px, scale);
    const int label_font = menu_font(false, hint_label_font_px, scale);
    const int cap_h = s(hint_cap_h);
    const float arm = chevron_arm * scale;
    const float arrows_gap = hint_arrows_gap * scale;
    const bool arrows = hint.arrows != MenuHintArrows::None;
    const int content_w = arrows ? static_cast<int>(std::ceil(hint_arrows_w(hint.arrows, arm, arrows_gap)))
                                 : rf::gr::get_string_size(hint.key, key_font).first;
    const int cap_w = std::max(cap_h, content_w + 2 * s(hint_cap_pad));
    const int label_x = x + cap_w + s(hint_label_gap);
    const int w = label_x + rf::gr::get_string_size(hint.label, label_font).first - x;
    if (!draw) {
        return w;
    }
    const int cap_y = mid - cap_h / 2;
    const float radius = hint_cap_radius * scale;
    ui_rounded_rect(static_cast<float>(x), static_cast<float>(cap_y), static_cast<float>(cap_w),
                    static_cast<float>(cap_h), radius, skin::steel_dark);
    ui_rounded_rect_border(static_cast<float>(x), static_cast<float>(cap_y), static_cast<float>(cap_w),
                           static_cast<float>(cap_h), radius, std::max(1.0f, scale), skin::steel_mid);
    if (arrows) {
        draw_hint_arrows(hint.arrows, static_cast<float>(x) + static_cast<float>(cap_w) / 2.0f,
                         static_cast<float>(mid), arm, arrows_gap, std::max(1.0f, 2.0f * scale));
    }
    else {
        rf::gr::set_color(skin::txt_hi);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, x + cap_w / 2,
                               cap_y + (cap_h - rf::gr::get_font_height(key_font)) / 2, hint.key.c_str(), key_font);
    }
    rf::gr::set_color(skin::txt);
    rf::gr::string(label_x, mid - rf::gr::get_font_height(label_font) / 2, hint.label.c_str(), label_font);
    return w;
}

// From `x` the others up to `limit`, then Esc's: the others that would leave it no room are dropped.
void draw_hints(const UiContext& ctx, std::span<const MenuHint> hints, int x, int mid, int limit)
{
    const int gap = skin_px(ctx, hint_gap);
    const auto esc = std::ranges::find_if(hints, is_esc_hint);
    const int esc_w = esc != hints.end() ? build_hint(ctx, *esc, 0, mid, false) : 0;
    const int others_limit = esc != hints.end() ? limit - esc_w - gap : limit;
    for (const MenuHint& hint : hints) {
        if (is_esc_hint(hint)) {
            continue;
        }
        const int w = build_hint(ctx, hint, x, mid, false);
        if (x + w > others_limit) {
            break;
        }
        build_hint(ctx, hint, x, mid, true);
        x += w + gap;
    }
    if (esc != hints.end() && x + esc_w <= limit) {
        build_hint(ctx, *esc, x, mid, true);
    }
}

// A step of the path strip as drawn: `index` into the path, past its end for the ellipsis.
struct PathStep
{
    std::size_t index = 0;
    std::string text;
    UiRect rect;
};

// The path strip's lettering, against the whole screen: the player's portrait against the right end and the name
// before it unless it is still stock's default; then the path's steps from after the bar, with a chevron between
// them: the nearest that fit before the name, an ellipsis for those dropped from the left.
struct PathStripLayout
{
    int mid = 0;
    int font = 0;
    std::vector<PathStep> steps;
    UiRect portrait;
    SkinText name{};
    UiRect name_rect;
    // The name and the portrait, the strip's height.
    UiRect player;
};

PathStripLayout path_strip_layout(const UiContext& ctx, std::span<const std::string> path)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const int strip_h = s(menu_strip_h);
    const int right = rf::gr::clip_width() - s(strip_pad);
    const int x0 = s(bar_w) + s(strip_pad);
    PathStripLayout strip;
    strip.mid = strip_h / 2;
    strip.font = menu_font(true, path_font_px, ctx.scale());
    int room_r = right;
    if (rf::local_player) {
        const int inset = s(portrait_inset);
        const int size = strip_h - 2 * inset;
        strip.portrait = UiRect{right - size, inset, size, size};
        strip.player = UiRect{strip.portrait.x, 0, size, strip_h};
        if (const char* name = rf::local_player->name.c_str(); player_name_configured(name)) {
            strip.name = skin_fit_text(ctx, name, menu_font(false, path_font_px, ctx.scale()), s(player_name_max_w));
            const int w = rf::gr::get_string_size(strip.name.text, strip.name.font).first;
            const int h = rf::gr::get_font_height(strip.name.font);
            strip.name_rect = UiRect{strip.portrait.x - s(player_name_gap) - w, strip.mid - h / 2, w, h};
            strip.player = UiRect{strip.name_rect.x, 0, right - strip.name_rect.x, strip_h};
        }
        room_r = strip.player.x - s(path_player_gap);
    }
    if (path.empty()) {
        return strip;
    }
    const int step = s(chevron_step);
    const auto text_w = [&](std::string_view text) { return rf::gr::get_string_size(text, strip.font).first; };
    int room = room_r - x0;
    const int ellipsis_w = text_w("...") + step;
    std::size_t first = path.size() - 1;
    int used = text_w(path[first]);
    while (first > 0 && used + step + text_w(path[first - 1]) + (first > 1 ? ellipsis_w : 0) <= room) {
        used += step + text_w(path[--first]);
    }
    const int text_h = rf::gr::get_font_height(strip.font);
    int x = x0;
    const auto add = [&](std::size_t index, std::string text) {
        const int w = text_w(text);
        strip.steps.push_back(PathStep{index, std::move(text), UiRect{x, strip.mid - text_h / 2, w, text_h}});
        x += w + step;
    };
    if (first > 0) {
        add(path.size(), "...");
        room -= ellipsis_w;
    }
    for (std::size_t i = first; i + 1 < path.size(); ++i) {
        add(i, path[i]);
    }
    // Only the last step can be too long alone.
    add(path.size() - 1, first + 1 == path.size() ? ui_fit_end(path.back(), std::max(1, room), strip.font)
                                                  : path.back());
    return strip;
}

// A step's click target: its lettering, the strip's height.
UiRect path_step_target(const UiContext& ctx, const PathStep& step)
{
    return UiRect{step.rect.x, 0, step.rect.w, skin_px(ctx, menu_strip_h)};
}

bool path_step_linked(const PathStep& step, const MenuStripLinks& links)
{
    return step.index < links.steps;
}

// Without a portrait, the character's initial, as a picker tile without one shows its name.
void draw_player_portrait(const UiContext& ctx, const UiRect& r, int font, bool lit)
{
    const int character = mp_character_selected();
    const int bm = character >= 0 ? rf::mp_characters[character].image_handle : -1;
    if (const char* caption = menu_draw_portrait(ctx, r, bm) ? nullptr : mp_character_caption(character)) {
        const UiRect image{r.x + 1, r.y + 1, r.w - 2, r.h - 2};
        const char initial[2]{static_cast<char>(std::toupper(static_cast<unsigned char>(caption[0]))), '\0'};
        rf::gr::set_color(skin::txt_hint);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, image.x + image.w / 2,
                               image.y + (image.h - rf::gr::get_font_height(font)) / 2, initial, font);
    }
    if (lit) {
        rf::gr::set_color(skin::txt_hi);
        hud_rect_border(r.x, r.y, r.w, r.h, skin_px(ctx, 2.0f));
    }
}

void draw_path_strip(const UiContext& ctx, std::span<const std::string> path, const MenuStripLinks& links)
{
    const PathStripLayout strip = path_strip_layout(ctx, path);
    const float arm = chevron_arm * ctx.scale();
    const float line_w = std::max(1.0f, 2.0f * ctx.scale());
    for (const PathStep& step : strip.steps) {
        const bool last = &step == &strip.steps.back();
        const bool lit = path_step_linked(step, links) && ui_hover(ctx, path_step_target(ctx, step));
        rf::gr::set_color(last || lit ? skin::ink_title : skin::ink_hint);
        rf::gr::string(step.rect.x, step.rect.y, step.text.c_str(), strip.font);
        if (!last) {
            draw_chevron(static_cast<float>(step.rect.x + step.rect.w + skin_px(ctx, chevron_gap)) + arm,
                         static_cast<float>(strip.mid), 1.0f, 0.0f, arm, line_w, skin::ink_dim);
        }
    }
    if (strip.portrait.w <= 0) {
        return;
    }
    const bool lit = links.player && ui_hover(ctx, strip.player);
    draw_player_portrait(ctx, strip.portrait, strip.font, lit);
    if (!strip.name.text.empty()) {
        rf::gr::set_color(lit ? skin::ink_title : skin::ink_status);
        rf::gr::string(strip.name_rect.x, strip.name_rect.y, strip.name.text.c_str(), strip.name.font);
    }
}

// A column label breaks at its spaces, so its widest word is what has to fit.
void note_widest_word(std::string_view text, int font, std::string_view& widest, int& widest_w)
{
    while (!text.empty()) {
        const std::size_t end = text.find(' ');
        const std::string_view word = text.substr(0, end);
        const int w = rf::gr::get_string_size(word, font).first;
        if (w > widest_w) {
            widest = word;
            widest_w = w;
        }
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    }
}

} // namespace

bool menu_draw_portrait(const UiContext& ctx, const UiRect& r, int bm)
{
    skin_slot(ctx, r.x, r.y, r.w, r.h);
    int bw = 0;
    int bh = 0;
    if (bm >= 0) {
        rf::bm::get_dimensions(bm, &bw, &bh);
    }
    const int side = std::min(bw, bh);
    if (side <= 0) {
        return false;
    }
    rf::gr::set_color(255, 255, 255, 255);
    rf::gr::bitmap_scaled(bm, r.x + 1, r.y + 1, r.w - 2, r.h - 2, (bw - side) / 2, 0, side, side, false, false,
                          rf::gr::bitmap_clamp_mode);
    return true;
}

std::string menu_stock_label(const char* text)
{
    std::string label;
    for (const char c : string_to_upper(text)) {
        const bool space = c == ' ' || c == '\n' || c == '\r';
        if (!space) {
            label += c;
        }
        else if (!label.empty() && label.back() != ' ') {
            label += ' ';
        }
    }
    if (!label.empty() && label.back() == ' ') {
        label.pop_back();
    }
    return label;
}

float menu_screen_scale()
{
    return std::min(rf::gr::clip_width() / menu_ref_w, rf::gr::clip_height() / menu_ref_h);
}

MenuFrame menu_frame(UiContext& ctx, float slide)
{
    const float scale = menu_screen_scale();
    ctx.set_scale(scale);
    MenuFrame f;
    f.canvas_y = (rf::gr::clip_height() - static_cast<int>(menu_ref_h * scale)) / 2;
    f.lay = menu_layout(ctx);
    // Far enough that the connector on the panel's left edge leaves the screen too.
    f.slide_w = rf::gr::clip_width() - f.lay.panel_x + skin_px(ctx, menu_connector_w);
    f.enter_dx = static_cast<int>((1.0f - slide) * static_cast<float>(f.slide_w));
    f.col_dx = -static_cast<int>((1.0f - slide) * static_cast<float>(f.lay.column_x + f.lay.column_w));
    f.top = f.canvas_y + ctx.scaled(menu_column_y);
    f.fonts = {menu_font(true, 20.0f, scale), menu_font(true, 32.0f, scale), menu_font(true, 26.0f, scale),
               menu_font(false, 20.0f, scale), menu_font(false, 17.0f, scale)};
    return f;
}

int menu_font(bool bold, float ref_px, float scale)
{
    // TTF sizes for a window under 480 px are unreadable, so stock keeps its bitmap fonts there.
    if (rf::gr::clip_height() >= 480) {
        const int px = std::max(8, static_cast<int>(std::lround(ref_px * scale)));
        const int font = ui_ttf_font(bold, px);
        if (font >= 0) {
            return font;
        }
    }
    return bold ? rf::ui::large_font : rf::ui::medium_font_0;
}

MenuLayout menu_layout(const UiContext& ctx)
{
    const int bar = skin_px(ctx, bar_w);
    MenuLayout lay;
    lay.column_x = bar + skin_px(ctx, plate_shown_w);
    lay.column_hidden = lay.column_x - (bar - skin_px(ctx, bar_tuck));
    lay.column_w = skin_px(ctx, well_visible_w) + skin_px(ctx, skin::menu_cap_w);
    lay.panel_x = lay.column_x + lay.column_w + skin_px(ctx, menu_connector_w);
    lay.panel_r = std::min(rf::gr::clip_width() - skin_px(ctx, right_gutter), lay.panel_x + skin_px(ctx, panel_max_w));
    return lay;
}

int menu_button_h(const UiContext& ctx)
{
    // Every small slot uses the same rounded height, so anything lined up with a button lines up exactly.
    return skin_px(ctx, skin::menu_button_h);
}

int menu_slot_y(const UiContext& ctx, MenuButtonSize size, int slot)
{
    // Large slots are cut from the span the panel's frame is drawn to, so however the heights round, the last one
    // ends with the frame.
    const int small_h = menu_button_h(ctx);
    return size == MenuButtonSize::Large ? slot * menu_panel_slots * small_h / menu_large_slots : slot * small_h;
}

int menu_slot_mid_y(const UiContext& ctx, MenuButtonSize size, int slot)
{
    return (menu_slot_y(ctx, size, slot) + menu_slot_y(ctx, size, slot + 1)) / 2;
}

float menu_slide_step()
{
    // Floored so a stalled frametime can never park a slide short of its end.
    return std::max(rf::frametime, min_animation_step) / slide_seconds;
}

UiRect menu_draw_title_bar(const UiContext& ctx, int x, int right, int canvas_y, const char* title)
{
    const auto s = [&](float v) { return ctx.scaled(v); };
    const UiRect bar{x, canvas_y + s(title_bar_y), right - x, s(title_bar_h)};
    skin_brushed_plate(ctx, bar.x, bar.y, bar.w, bar.h, false);
    const int block_w = s(title_bar_h);
    skin_brushed_plate(ctx, bar.x, bar.y, block_w, bar.h, true);
    const int mid_y = bar.y + bar.h / 2;
    skin_screw(ctx, bar.x + block_w / 2, mid_y, 20.0f);
    skin_screw(ctx, right - s(30.0f), mid_y, 20.0f);

    const int font = menu_font(true, title_font_px, ctx.scale());
    const int title_x = x + s(title_inset);
    rf::gr::set_color(skin::ink_title);
    rf::gr::string(title_x, mid_y - rf::gr::get_font_height(font) / 2, title, font);

    const int free_l = title_x + rf::gr::get_string_size(title, font).first + s(40.0f);
    const int free_r = right - s(60.0f);
    return UiRect{free_l, bar.y, std::max(0, free_r - free_l), bar.h};
}

MenuPanel menu_panel(const UiContext& ctx, const MenuLayout& lay, int dx, int top, int canvas_y)
{
    const auto s = [&](float v) { return ctx.scaled(v); };
    const int h = menu_panel_slots * menu_button_h(ctx);
    MenuPanel panel;
    panel.frame = UiRect{lay.panel_x + dx, top, lay.panel_r - lay.panel_x, h};
    const int inset_x = panel.frame.x + s(frame_left);
    panel.inset = UiRect{inset_x, top + s(frame_edge), panel.frame.x + panel.frame.w - s(content_clearance) - inset_x,
                         h - 2 * s(frame_edge)};
    panel.content_x = panel.inset.x + s(20.0f);
    panel.content_r = panel.inset.x + panel.inset.w - s(16.0f);
    const int content_w = panel.content_r - panel.content_x;
    panel.header = UiRect{panel.content_x, canvas_y + s(menu_panel_header_y), content_w, s(menu_panel_header_h)};
    const int body_y = canvas_y + s(menu_panel_body_y);
    panel.body = UiRect{panel.content_x, body_y, content_w, panel.inset.y + panel.inset.h - s(20.0f) - body_y};
    return panel;
}

void menu_draw_panel_ridge(const UiContext& ctx, const MenuPanel& panel)
{
    skin_ridge(ctx, panel.inset.x + ctx.scaled(8.0f), panel.header.y + panel.header.h + ctx.scaled(1.0f),
               panel.inset.w - ctx.scaled(16.0f));
}

void menu_draw_panel_heading(const UiContext& ctx, const MenuPanel& panel, const char* text, int font)
{
    const UiRect& header = panel.header;
    rf::gr::set_color(skin::ink_title);
    rf::gr::string(header.x, header.y + (header.h - rf::gr::get_font_height(font)) / 2, text, font);
    menu_draw_panel_ridge(ctx, panel);
}

void menu_draw_panel(const UiContext& ctx, const MenuPanel& panel)
{
    const UiRect& f = panel.frame;
    skin_brushed_plate(ctx, f.x, f.y, f.w, f.h, false);
    const int margin = skin_px(ctx, frame_margin);
    skin_brushed_plate(ctx, f.x + margin, f.y + margin, skin_px(ctx, frame_channel_w), f.h - 2 * margin, true);

    const int t = skin_px(ctx, 1.0f);
    const int ridge_x = f.x + skin_px(ctx, 30.0f);
    // The right end is in view, so the ridges stop at its margin.
    const int ridge_w = f.x + f.w - margin - ridge_x;
    for (const float y_ref : {7.0f, 13.0f}) {
        for (const int y : {f.y + skin_px(ctx, y_ref), f.y + f.h - skin_px(ctx, y_ref + 2.0f)}) {
            rf::gr::set_color(skin::steel_dark);
            rf::gr::rect(ridge_x, y, ridge_w, t);
            rf::gr::set_color(skin::steel_hi);
            rf::gr::rect(ridge_x, y + t, ridge_w, t);
        }
    }

    skin_steel_field(ctx, panel.inset.x, panel.inset.y, panel.inset.w, panel.inset.h);
}

void menu_draw_connector(const UiContext& ctx, const MenuPanel& panel, int mid_y)
{
    const int cw = skin_px(ctx, menu_connector_w);
    const int ch = skin_px(ctx, 60.0f);
    skin_sized_chrome("af_connector", UiRect{panel.frame.x - cw, mid_y - ch / 2, cw, ch});
}

void menu_draw_modal_dim()
{
    rf::gr::set_color(modal_dim);
    rf::gr::rect(0, 0, rf::gr::clip_width(), rf::gr::clip_height());
}

std::string menu_root_crumb()
{
    // Stock names only the title menu.
    return rf::ui::mainmenu_game_beneath ? std::string{"GAME MENU"} : menu_stock_label(rf::strings::main_menu);
}

int menu_own_state()
{
    const int top = std::min(rf::gameseq_stack_top_index, static_cast<int>(std::size(rf::gameseq_state_stack)) - 1);
    // The topmost menu state.
    int own = top;
    while (own >= 0 && state_crumb(rf::gameseq_state_stack[own]).empty()) {
        --own;
    }
    return own;
}

MenuCrumbTarget menu_crumb_target(std::size_t step)
{
    const int own = menu_own_state();
    std::size_t crumbs = 0;
    for (int i = 0; i < own; ++i) {
        if (!state_crumb(rf::gameseq_state_stack[i]).empty() && crumbs++ == step) {
            return MenuCrumbTarget{i, 0};
        }
    }
    return MenuCrumbTarget{own, step - crumbs};
}

bool menu_player_reachable()
{
    // Over a single player game no menu opens the Multi screen.
    return rf::local_player && (!rf::ui::mainmenu_game_beneath || rf::is_multi);
}

std::vector<std::string> menu_crumbs(std::span<const std::string> hosts)
{
    // `hosts` stand in for the state the screen runs in.
    const int own = menu_own_state();
    std::vector<std::string> crumbs;
    for (int i = 0; i < own; ++i) {
        if (std::string crumb = state_crumb(rf::gameseq_state_stack[i]); !crumb.empty()) {
            crumbs.push_back(std::move(crumb));
        }
    }
    crumbs.insert(crumbs.end(), hosts.begin(), hosts.end());
    return crumbs;
}

void menu_add_nav_hints(const UiContext& ctx, std::vector<MenuHint>& hints, std::string esc, bool nav)
{
    if (ctx.focused_text_id() != 0) {
        // A box without a commit edits live, and Esc keeps what was typed as Enter does.
        hints.push_back({"ESC", ctx.has_text_commit() ? "Cancel" : "Done"});
        hints.push_back({"ENTER", "Done"});
        return;
    }
    if (ctx.popup_id() != 0) {
        esc = "Close";
    }
    if (!esc.empty()) {
        hints.push_back({"ESC", std::move(esc)});
    }
    if (!nav) {
        return;
    }
    hints.push_back({{}, "Move", MenuHintArrows::UpDown});
    if (ctx.focus_adjustable()) {
        hints.push_back({{}, "Adjust", MenuHintArrows::LeftRight});
    }
    // A focus whose widget is gone, such as a closed panel's row, has nothing for Enter to select.
    if (ctx.focus_registered() || ctx.popup_id() != 0) {
        hints.push_back({"ENTER", "Select"});
    }
}

void menu_draw_frame(const UiContext& ctx, std::span<const std::string> path, std::span<const MenuHint> hints,
                     const MenuStripLinks& links)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    rf::gr::reset_clip();
    const StripLayout strip = strip_layout(ctx);
    const int w = rf::gr::clip_width();
    const int h = rf::gr::clip_height();
    const int bar = s(bar_w);
    const int strip_h = s(menu_strip_h);
    const int mid = strip.mid;

    skin_brushed_plate(ctx, 0, 0, w, strip_h, false);
    draw_path_strip(ctx, path, links);
    g_drawn_strip = DrawnStrip{rf::frame_count, strip_h};

    // The hint bar: dark steel, edged along its top.
    rf::gr::set_color(skin::steel_edge);
    rf::gr::rect(0, h - strip_h, w, strip_h);
    rf::gr::set_color(skin::steel_dark);
    rf::gr::rect(0, h - strip_h, w, s(1.0f));
    draw_hints(ctx, hints, strip.hints_x, mid, strip.hints_r);
    if (!strip.context.text.empty()) {
        const UiRect& r = strip.context_rect;
        rf::gr::set_color(skin::txt);
        rf::gr::string(r.x, r.y, strip.context.text.c_str(), strip.context.font);
    }

    // Over the strips' ends and the buttons' tucked ends: a panel frame's left edge, the screen's height, with a screw
    // level with each strip's lettering.
    skin_brushed_plate(ctx, 0, 0, bar, h, false);
    const int margin = s(frame_margin);
    const int channel_w = s(frame_channel_w);
    skin_brushed_plate(ctx, margin, margin, channel_w, h - 2 * margin, true);
    for (const int y : {strip_h / 2, mid}) {
        skin_screw(ctx, margin + channel_w / 2, y, bar_screw_size);
    }
}

int menu_strip_click(const UiContext& ctx, std::span<const std::string> path, const MenuStripLinks& links)
{
    if (!ctx.click || (links.steps == 0 && !links.player)) {
        return -1;
    }
    const PathStripLayout strip = path_strip_layout(ctx, path);
    if (links.player && strip.portrait.w > 0 && ui_hover(ctx, strip.player)) {
        return menu_strip_player;
    }
    for (const PathStep& step : strip.steps) {
        if (path_step_linked(step, links) && ui_hover(ctx, path_step_target(ctx, step))) {
            return static_cast<int>(step.index);
        }
    }
    return -1;
}

UiRect menu_strip_context_rect(const UiContext& ctx)
{
    return strip_layout(ctx).context_rect;
}

int menu_strip_drawn_h()
{
    // Last frame's too, so a state change's frame doesn't drop the offset.
    const int age = rf::frame_count - g_drawn_strip.frame;
    return age >= 0 && age <= 1 ? g_drawn_strip.h : 0;
}

MenuDialog menu_dialog(const UiContext& ctx, int w, int h, const char* title, int title_font)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const float scale = menu_screen_scale();
    const int canvas_w = static_cast<int>(menu_ref_w * scale);
    const int canvas_h = static_cast<int>(menu_ref_h * scale);
    const int canvas_x = (rf::gr::clip_width() - canvas_w) / 2;
    const int canvas_y = (rf::gr::clip_height() - canvas_h) / 2;
    MenuDialog dialog;
    dialog.frame = UiRect{canvas_x + (canvas_w - w) / 2, canvas_y + (canvas_h - h) / 2, w, h};
    const int edge = s(1.0f);
    dialog.band = UiRect{dialog.frame.x + edge, dialog.frame.y + edge, dialog.frame.w - 2 * edge, s(56.0f)};
    if (ctx.draw) {
        menu_draw_modal_dim();
        skin_popup(ctx, dialog.frame, dialog.band, title, title_font);
    }
    return dialog;
}

MenuCard menu_card(const UiContext& ctx, const UiRect& bounds, float ref_w)
{
    const int pad = ctx.scaled(12.0f);
    const int w = std::min(std::max(ctx.scaled(ref_w), 380), bounds.w - 2 * pad);
    return MenuCard{w, pad, w - 2 * pad};
}

UiRect menu_card_place(const UiContext& ctx, const MenuCard& card, int h, const UiRect& row, int anchor_x,
                       const UiRect& bounds)
{
    const int pad = card.pad;
    const int right = bounds.x + bounds.w - pad;
    const int bottom = bounds.y + bounds.h - pad;
    UiRect r{std::clamp(anchor_x, bounds.x + pad, std::max(bounds.x + pad, right - card.w)),
             row.y + row.h + ctx.scaled(6.0f), card.w, h};
    if (r.y + h > bottom) {
        r.y = row.y - ctx.scaled(6.0f) - h;
    }
    if (r.y < bounds.y + pad) {
        r.y = std::clamp(row.y + row.h / 2 - h / 2, bounds.y + pad, std::max(bounds.y + pad, bottom - h));
        const int left = bounds.x + pad;
        const int after = anchor_x + ctx.scaled(24.0f);
        const int before = anchor_x - ctx.scaled(24.0f) - card.w;
        const bool fits_after = after + card.w <= right;
        const bool fits_before = before >= left;
        // Fitting neither, the roomier side, covering the anchor as little as it can.
        const bool use_after = fits_after || (!fits_before && right - after > before + card.w - left);
        r.x = std::max(left, use_after ? std::min(after, right - card.w) : before);
    }
    return r;
}

void menu_draw_card(const UiContext& ctx, const UiRect& r)
{
    rf::gr::set_color(0, 0, 0, 255);
    rf::gr::rect(r.x, r.y, r.w, r.h);
    rf::gr::set_color(skin::border);
    hud_rect_border(r.x, r.y, r.w, r.h, std::max(1, ctx.scaled(1.0f)));
}

void menu_draw_level_preview(const std::string& level_filename, const UiRect& r, const MenuLevelCaption& caption)
{
    if (r.h > caption.cap_h) {
        map_thumbnail_draw(level_filename, r.x, r.y, r.w, r.h);
    }
    const int cap_y = r.y + r.h - caption.cap_h;
    rf::gr::set_color(0, 0, 0, 170);
    rf::gr::rect(r.x, cap_y, r.w, caption.cap_h);
    const int text_y = cap_y + (caption.cap_h - rf::gr::get_font_height(caption.font)) / 2;
    const std::string type = ui_fit_end(caption.type, r.w / 2, caption.font);
    rf::gr::set_color(caption.type_color);
    rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, r.x + r.w - caption.pad, text_y, type.c_str(), caption.font);
    const int level_w = r.w - rf::gr::get_string_size(type, caption.font).first - 3 * caption.pad;
    skin_draw_text(r.x + caption.pad, text_y, ui_fit_end(caption.level, level_w, caption.font), caption.font,
                   caption.level_color);
}

int menu_build_column(UiContext& ctx, const MenuLayout& lay, int x, int y, std::span<const MenuColumnItem> items,
                      const char* back_label, int font, const char* id_name, MenuButtonSize size)
{
    const bool tall = size == MenuButtonSize::Large;
    // Every label takes the size the widest word fits at, so the column reads as one set; a label
    // too long for one line breaks onto two rather than shrinking the rest.
    std::string_view widest = back_label;
    int widest_w = 0;
    note_widest_word(back_label, font, widest, widest_w);
    for (const MenuColumnItem& item : items) {
        note_widest_word(item.label, font, widest, widest_w);
    }
    font = skin_fit_text(ctx, widest, font, skin_menu_button_label_w(ctx, lay.column_w)).font;

    const auto slot_rect = [&](std::size_t slot) {
        const int top = menu_slot_y(ctx, size, static_cast<int>(slot));
        return UiRect{x, y + top, lay.column_w, menu_slot_y(ctx, size, static_cast<int>(slot) + 1) - top};
    };
    int clicked = -1;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (skin_menu_button(ctx, slot_rect(i), lay.column_hidden, items[i].label.c_str(), font, items[i].selected,
                             items[i].arrow, tall, ui_id(id_name, static_cast<int>(i)), items[i].enabled)) {
            clicked = static_cast<int>(i);
        }
    }
    if (skin_menu_button(ctx, slot_rect(items.size()), lay.column_hidden, back_label, font, false, false, tall,
                         ui_id("back"), true)) {
        clicked = static_cast<int>(items.size());
    }
    return clicked;
}

void menu_ui_reset(UiContext& ctx)
{
    ctx.reset();
    ctx.style = settings_menu_style();
    ctx.set_hover_sound_enabled(true);
}

void menu_run_hit_test(UiContext& ctx, void (*key_callback)(int), const std::function<void(UiContext&)>& build)
{
    if (key_callback) {
        rf::game_poll(key_callback);
    }
    ui_drop_text_focus_under_console(ctx);
    int x = 0;
    int y = 0;
    int z = 0;
    rf::mouse_get_pos(x, y, z);
    ctx.begin_frame(x, y, rf::mouse_dz, rf::mouse_button_is_down(0), rf::mouse_was_button_pressed(0) != 0,
                    rf::mouse_was_button_pressed(1) != 0, rf::mouse_was_button_pressed(2) != 0);
    ctx.begin_pass(UiPass::HitTest);
    build(ctx);
    ctx.end_pass();
}

void menu_run_draw(UiContext& ctx, const std::function<void(UiContext&)>& build, bool backdrop)
{
    rf::gr::reset_clip();
    if (backdrop) {
        rf::ui::menu_draw_background();
    }
    int x = 0;
    int y = 0;
    int z = 0;
    rf::mouse_get_pos(x, y, z);
    ctx.set_mouse_pos(x, y);
    ctx.begin_pass(UiPass::Draw);
    build(ctx);
    ctx.end_pass();
    ctx.draw_overlays();
    rf::gr::reset_clip();
}
