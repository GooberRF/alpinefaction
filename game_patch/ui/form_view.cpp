#include <algorithm>
#include <common/utils/int-utils.h>
#include <common/utils/string-utils.h>
#include "form_view.h"
#include "settings_skin.h"
#include "../graphics/ui_draw.h"
#include "../rf/gr/gr_font.h"

namespace
{

constexpr float crumb_gap = 4.0f;
constexpr float sub_option_indent = 40.0f;

} // namespace

FormFonts form_fonts(const MenuFonts& fonts)
{
    return FormFonts{fonts.body, fonts.column, fonts.caption};
}

FormLine form_row(UiContext& ctx, const FormRow& row, const FormFonts& fonts)
{
    const UiRect& r = row.rect;
    const int pad = skin_px(ctx, form_row_inset);
    const int gap = skin_px(ctx, 12.0f);
    const int reset_w = skin_px(ctx, skin::icon_size);

    const bool hovered = ui_hover(ctx, r);
    const bool focused = ctx.focus_visible() && ctx.focus_id() == row.id;
    const int ctl_right = r.x + r.w - pad;
    const int line_ctl_right = ctl_right - reset_w - gap;
    if (!ctx.draw && row.desc_id >= 0) {
        if (hovered || focused) {
            ctx.description().hover(row.desc_id);
        }
        // A click anywhere on the row pins its description until the strip moves on.
        if (hovered && ctx.click) {
            ctx.description().pin(row.desc_id);
        }
    }

    if (ctx.draw) {
        const UiRect d = ui_draw_rect(ctx, r);
        form_draw_row_backing(ctx, d, focused);

        int tx = d.x + pad;
        if (row.tree != FormTree::None) {
            const int t = std::max(1, skin_px(ctx, 2.0f));
            const int stem_x = tx + skin_px(ctx, 8.0f);
            const int mid_y = d.y + d.h / 2;
            rf::gr::set_color(skin::steel_dark);
            rf::gr::rect(stem_x, d.y, t, row.tree == FormTree::Last ? mid_y - d.y + t : d.h);
            rf::gr::rect(stem_x, mid_y, skin_px(ctx, 14.0f), t);
            tx += skin_px(ctx, sub_option_indent);
        }
        const int label_h = rf::gr::get_font_height(fonts.body);
        // The label and any crumb under it as one block, centered on the row.
        const int label_y = d.y + (d.h - label_h - (row.crumb ? form_crumb_line_h(ctx, fonts.caption) : 0)) / 2;
        // The lamp and the badges go on the label's line.
        const int line_y = row.crumb ? label_y : d.y;
        const int line_h = row.crumb ? label_h : d.h;
        if (row.modified) {
            // In the margin, so the label stays in line with the others; clear of the focus LED.
            const int dot = skin_px(ctx, 8.0f);
            ui_rounded_rect(static_cast<float>(tx - skin_px(ctx, 6.0f) - dot),
                            static_cast<float>(line_y + (line_h - dot) / 2), static_cast<float>(dot),
                            static_cast<float>(dot), static_cast<float>(dot) * 0.5f, skin::amber);
        }
        // Clear of a control of the usual width, which a narrow form would otherwise run the label under.
        const int label_w = d.x + (line_ctl_right - r.x) - skin_px(ctx, skin::control_w) - gap - tx;
        const std::string label = ui_fit_end(row.label, std::max(0, label_w), fonts.body);
        rf::gr::set_color(form_label_color(row.enabled, hovered || row.modified, focused));
        rf::gr::string(tx, label_y, label.c_str(), fonts.body);
        if (row.crumb) {
            rf::gr::set_color(focused ? skin::txt : skin::ink_hint);
            rf::gr::string(tx, label_y + label_h + skin_px(ctx, crumb_gap), row.crumb, fonts.caption);
        }
        tx += rf::gr::get_string_size(label, fonts.body).first + gap;

        const int badge_h = skin_px(ctx, skin::badge_h);
        const int badge_y = r.y + (line_y - d.y) + (line_h - badge_h) / 2;
        for (const FormBadge& badge : row.badges) {
            const int w = skin_badge_width(ctx, badge.text, fonts.caption, badge.lamp || badge.icon);
            const UiRect br{r.x + (tx - d.x), badge_y, w, badge_h};
            skin_badge(ctx, br, badge.text, badge.color, fonts.caption, badge.icon, badge.lamp);
            tx += w + skin_px(ctx, 8.0f);
        }
    }

    FormLine line{r, line_ctl_right, row.enabled, hovered || focused, false};
    if (row.modified && row.enabled && row.reset_id != 0) {
        const UiRect reset_rect{ctl_right - reset_w, r.y + (r.h - reset_w) / 2, reset_w, reset_w};
        line.reset = skin_icon_button(ctx, reset_rect, "af_reset_icon", skin::amber, skin::ink_title, row.reset_id);
    }
    return line;
}

int form_crumb_line_h(const UiContext& ctx, int font)
{
    return skin_px(ctx, crumb_gap) + rf::gr::get_font_height(font);
}

void form_draw_row_backing(const UiContext& ctx, const UiRect& d, bool focused)
{
    // Faded out well before the controls.
    if (focused) {
        const int inset = skin_px(ctx, 3.0f);
        skin_selection_bar(ctx, d.x, d.y + inset, std::min(d.w, skin_px(ctx, 900.0f)), d.h - 2 * inset);
        return;
    }
    const int pad = skin_px(ctx, form_row_inset);
    skin_ridge(ctx, d.x + pad / 2, d.y + d.h - std::max(1, skin_px(ctx, 1.0f)), d.w - pad);
}

rf::Color form_label_color(bool enabled, bool hot, bool focused)
{
    // Dark ink on the steel, white on the focused row's selection glow.
    if (!enabled) {
        return skin::ink_dim;
    }
    if (focused) {
        return skin::txt_hi;
    }
    return hot ? skin::ink_title : skin::ink_status;
}

void form_draw_sub_header(const UiContext& ctx, const UiRect& r, const char* text, int font)
{
    if (!ctx.draw) {
        return;
    }
    const UiRect d = ui_draw_rect(ctx, r);
    skin_led(ctx, d.x + skin_px(ctx, 12.0f), d.y + d.h / 2, 7.0f, 22.0f, true);
    rf::gr::set_color(skin::ink_title);
    rf::gr::string(d.x + skin_px(ctx, form_row_inset), d.y + (d.h - rf::gr::get_font_height(font)) / 2,
                   string_to_upper(text ? text : "").c_str(), font);
}

UiRect form_control_rect(const UiContext& ctx, const FormLine& line, float w_ref, float h_ref)
{
    const UiRect& row = line.rect;
    const int w = skin_px(ctx, w_ref);
    const int h = h_ref > 0.0f ? skin_px(ctx, h_ref) : row.h;
    return UiRect{line.ctl_right - w, row.y + (row.h - h) / 2, w, h};
}

int form_cycler(UiContext& ctx, const FormLine& line, UiId id, const char* value, int font)
{
    const UiRect r = form_control_rect(ctx, line, skin::control_w, skin::widget_h);
    return skin_cycler(ctx, r, value, font, line.enabled, id, line.hot);
}

bool form_toggle(UiContext& ctx, const FormLine& line, UiId id, bool& on, int font)
{
    if (form_cycler(ctx, line, id, on ? "On" : "Off", font) == 0) {
        return false;
    }
    on = !on;
    ui_play_toggle(on);
    return true;
}

bool form_choice(UiContext& ctx, const FormLine& line, UiId id, const std::vector<std::string>& items, int& index,
                 int font)
{
    const int count = static_cast<int>(items.size());
    if (count > form_cycler_max_options) {
        return form_dropdown(ctx, line, id, items, index, font);
    }
    if (count == 0) {
        form_cycler(ctx, line, id, "-", font);
        return false;
    }
    const int shown = std::clamp(index, 0, count - 1);
    const int delta = form_cycler(ctx, line, id, items[shown].c_str(), font);
    if (delta == 0 || !line.enabled) {
        return false;
    }
    index = wrap_step(shown, delta, count);
    ui_play_click();
    return true;
}

bool form_dropdown(UiContext& ctx, const FormLine& line, UiId id, const std::vector<std::string>& items, int& index,
                   int font)
{
    const UiRect r = form_control_rect(ctx, line, skin::control_w, skin::widget_h);
    int sel = index;
    if (!skin_dropdown(ctx, r, items, sel, font, line.enabled, id, line.hot) || !line.enabled || sel == index) {
        return false;
    }
    index = sel;
    return true;
}

bool form_select_box(UiContext& ctx, const FormLine& line, UiId id, const char* text, int font)
{
    const UiRect r = form_control_rect(ctx, line, skin::control_w, skin::widget_h);
    return skin_select_box(ctx, r, text, font, line.enabled, id, line.hot);
}

bool form_slider(UiContext& ctx, const FormLine& line, UiId id, float& value, float min_value, float max_value,
                 float step, float nudge, const UiNumberFormat& fmt, int font, int readout_w)
{
    // The full row's height, so the nub has room above and below the track.
    const UiRect r = form_control_rect(ctx, line, skin::control_w, 0.0f);
    return skin_slider(ctx, r, value, min_value, max_value, step, nudge, fmt, font, readout_w, line.enabled, id,
                       line.hot);
}

bool form_stepper(UiContext& ctx, const FormLine& line, UiId id, float& value, float min_value, float max_value,
                  float step, const UiNumberFormat& fmt, int font, bool integral)
{
    const UiRect r = form_control_rect(ctx, line, skin::control_w, skin::widget_h);
    return skin_stepper(ctx, r, value, min_value, max_value, step, fmt, font, line.enabled, id, integral, line.hot);
}

void form_text(UiContext& ctx, const FormLine& line, UiId id, std::string& text, const FormTextSpec& spec, int font,
               UiId prev_text_focus, const std::function<void()>& on_commit)
{
    const UiRect r = form_control_rect(ctx, line, skin::control_w, 32.0f);
    skin_field(ctx, r, id, text, spec.max_len, spec.filter, spec.hint, font, line.enabled,
               line.enabled && spec.warn != nullptr, spec.warn);
    // Committed when the box lets the focus go, never per keystroke.
    if (!ctx.draw && line.enabled && ctx.focused_text_id() == id && prev_text_focus != id) {
        ctx.set_text_commit_callback([on_commit](bool commit) {
            if (commit) {
                on_commit();
            }
        });
    }
}

bool form_number_text(UiContext& ctx, const FormLine& line, UiId id, const std::string& shown, const std::string& seed,
                      const FormTextSpec& spec, int font, UiId prev_text_focus,
                      const std::function<void(const std::string&)>& on_commit)
{
    // The context's buffer holds the edit; it binds on the pass after the box takes the focus.
    std::string idle = shown;
    std::string& text = ctx.focused_text_id() == id ? ctx.text_buffer() : idle;
    form_text(ctx, line, id, text, spec, font, prev_text_focus, [&ctx, on_commit] { on_commit(ctx.text_buffer()); });
    const bool gained = !ctx.draw && line.enabled && ctx.focused_text_id() == id && prev_text_focus != id;
    if (gained) {
        ctx.text_buffer() = seed;
    }
    return gained;
}

void form_value_text(const UiContext& ctx, const FormLine& line, const char* text, int font)
{
    if (!ctx.draw) {
        return;
    }
    const UiRect d = ui_draw_rect(ctx, line.rect);
    rf::gr::set_color(line.hot ? skin::ink_title : skin::ink_status);
    rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, d.x + (line.ctl_right - line.rect.x),
                           d.y + (d.h - rf::gr::get_font_height(font)) / 2, text, font);
}

void form_draw_description(const UiContext& ctx, const UiRect& strip, const std::string& text, int font)
{
    const auto s = [&](float v) { return ctx.scaled(v); };
    skin_slot(ctx, strip.x, strip.y, strip.w, strip.h);
    const int line_h = std::max(1, rf::gr::get_font_height(font) + s(6.0f));
    // In line with the row labels above, which start a row inset into a view 16 left of the strip.
    const int text_x = strip.x + s(form_row_inset - 16.0f);
    const int text_w = std::max(1, strip.x + strip.w - s(18.0f) - text_x);
    const int max_lines = std::max(1, (strip.h - 2 * s(12.0f)) / line_h);
    const int lines = std::clamp(static_cast<int>(ui_wrap_text(text, text_w, font).size()), 1, max_lines);
    const int block_h = lines * line_h;
    const int text_y = strip.y + (strip.h - block_h) / 2;
    rf::gr::set_color(skin::txt);
    ui_draw_wrapped(ctx, UiRect{text_x, text_y, text_w, block_h}, text, font, line_h);
}

FormPanelAreas form_panel_areas(const UiContext& ctx, const MenuPanel& panel, bool with_desc, bool with_footer)
{
    const auto s = [&](float v) { return ctx.scaled(v); };
    const int x = panel.content_x;
    const int w = panel.content_r - panel.content_x;
    const int footer_y = panel.inset.y + panel.inset.h - s(menu_panel_footer_inset);
    const int body_end = panel.body.y + panel.body.h;
    const int desc_y = with_footer ? footer_y - s(74.0f) : body_end - s(66.0f);
    const int body_bottom = with_desc ? desc_y - s(8.0f) : with_footer ? footer_y - s(8.0f) : body_end;
    return FormPanelAreas{
        UiRect{x, panel.body.y, w, std::max(0, body_bottom - panel.body.y)},
        UiRect{x, desc_y, w, s(66.0f)},
        UiRect{x, footer_y, w, s(menu_panel_footer_h)},
    };
}

UiRect form_rows_view(const UiContext& ctx, const UiRect& area)
{
    const int inset = ctx.scaled(16.0f);
    return UiRect{area.x - inset, area.y, area.w + inset, area.h};
}

FormScroll form_scroll_begin(UiContext& ctx, const UiRect& view, int content_h, float& scroll, int wheel_step, UiId id)
{
    FormScroll out;
    out.region = ui_scroll_begin(ctx, view, content_h, scroll, wheel_step, id, true);
    out.inner_w = view.w - ui_scrollbar_width(ctx) - ctx.scaled(8.0f);
    return out;
}

bool form_scroll_culled(const FormScroll& scroll, const UiRect& row)
{
    const UiRect& view = scroll.region.view;
    return row.y + row.h < view.y || row.y > view.y + view.h;
}

std::optional<UiRect> form_scroll_row(UiContext& ctx, FormScroll& scroll, int x, int& y, int h, UiId id,
                                      UiFocusKind kind)
{
    const UiRect rect{x, y, scroll.region.view.x + scroll.inner_w - x, h};
    y += h;
    if (id != 0 && ctx.focus_id() == id) {
        scroll.focus_rect = rect;
    }
    if (!form_scroll_culled(scroll, rect)) {
        return rect;
    }
    ctx.register_focusable(id, kind);
    return std::nullopt;
}

void form_scroll_end(UiContext& ctx, FormScroll& scroll, UiId& scrolled_to)
{
    UiScrollRegion& region = scroll.region;
    ui_scroll_end(ctx, region);
    ui_scroll_focus_into_view(ctx, region.view, scroll.focus_rect, *region.scroll, scrolled_to);
    if (ctx.draw) {
        skin_scrollbar(ctx, region.view, region.content_h, *region.scroll, ctx.captured_id() == region.id);
    }
}
