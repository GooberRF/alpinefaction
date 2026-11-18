#include <algorithm>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <patch_common/FunHook.h>
#include "menu_popup.h"
#include "menu_shell.h"
#include "settings_skin.h"
#include "../misc/misc.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/input.h"
#include "../rf/localize.h"
#include "../rf/ui.h"

namespace
{

const UiId input_box_id = ui_id("popup_input");

UiId button_id(int index)
{
    return ui_id("popup_button", index);
}

// The text's lines as stock's popups break them, blank ones kept.
std::vector<std::string_view> paragraphs(std::string_view text)
{
    std::vector<std::string_view> out;
    std::size_t pos = 0;
    while (true) {
        const std::size_t end = text.find('\n', pos);
        std::string_view paragraph =
            text.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
        if (paragraph.ends_with('\r')) {
            paragraph.remove_suffix(1);
        }
        out.push_back(paragraph);
        if (end == std::string_view::npos) {
            return out;
        }
        pos = end + 1;
    }
}

// Each paragraph wrapped to `max_w`.
std::vector<std::string> wrap_paragraphs(std::string_view text, int max_w, int font)
{
    std::vector<std::string> lines;
    if (text.empty()) {
        return lines;
    }
    for (const std::string_view paragraph : paragraphs(text)) {
        if (paragraph.empty()) {
            lines.emplace_back();
        }
        else {
            for (std::string& line : ui_wrap_text(paragraph, max_w, font)) {
                lines.push_back(std::move(line));
            }
        }
    }
    return lines;
}

int widest_paragraph(std::string_view text, int font)
{
    int widest = 0;
    for (const std::string_view paragraph : paragraphs(text)) {
        widest = std::max(widest, rf::gr::get_string_size(paragraph, font).first);
    }
    return widest;
}

// One button of a popup's row; a lamp marks a choice that commits.
struct MenuPopupRowButton
{
    std::string_view label;
    bool lamp = false;
};

// A popup's dialog, sized so its title, its text and its row of buttons all fit. The draw pass paints the dialog and
// the text; the caller builds the buttons and the field in their rects.
struct MenuPopupLayout
{
    std::vector<UiRect> buttons;
    // With `input`: the text box, under the text.
    UiRect field{};
    int body_font = 0;
    int button_font = 0;
};

MenuPopupLayout menu_popup_layout(UiContext& ctx, const std::string& title, std::string_view text,
                                  const std::vector<MenuPopupRowButton>& buttons, bool input)
{
    const float scale = menu_screen_scale();
    ctx.set_scale(scale);
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const int title_font = menu_font(true, 24.0f, scale);
    MenuPopupLayout lay;
    lay.body_font = menu_font(false, 20.0f, scale);
    lay.button_font = menu_font(true, 20.0f, scale);

    // One width for every button, so none shrinks its label to fit.
    const int count = static_cast<int>(buttons.size());
    int btn_w = 0;
    for (const MenuPopupRowButton& button : buttons) {
        const std::string label{button.label};
        btn_w = std::max(btn_w, skin_button_width(ctx, label.c_str(), lay.button_font, 150.0f)
                                    + (button.lamp ? s(30.0f) : 0));
    }
    const int btn_gap = s(16.0f);
    const int pad = s(14.0f);
    const int edge = s(1.0f);
    const int inset = s(28.0f);

    // The text sets the width within bounds; the title and the buttons widen it past them, up to the canvas.
    const int text_need = widest_paragraph(text, lay.body_font) + 2 * inset + 2 * pad;
    const int title_need = s(36.0f) + rf::gr::get_string_size(title, title_font).first + inset;
    const int row_need = (count > 0 ? btn_w * count + btn_gap * (count - 1) : 0) + 2 * pad;
    const int dialog_w = std::min(std::max({std::clamp(text_need, s(640.0f), s(960.0f)), title_need, row_need}),
                                  s(menu_ref_w - 32.0f));
    if (count > 0 && row_need > dialog_w) {
        btn_w = std::max(1, (dialog_w - 2 * pad - btn_gap * (count - 1)) / count);
    }
    const int buttons_w = count > 0 ? btn_w * count + btn_gap * (count - 1) : 0;

    const int text_w = dialog_w - 2 * pad - 2 * inset;
    const int line_h = rf::gr::get_font_height(lay.body_font) + s(6.0f);
    const std::vector<std::string> lines = wrap_paragraphs(text, text_w, lay.body_font);
    const int field_h = s(skin::widget_h);
    const int field_gap = lines.empty() ? 0 : s(12.0f);
    const int band_h = s(56.0f);
    const int btn_h = s(44.0f);
    // The text the canvas cannot fit is cut, its last line ending in "...".
    const int fixed_h = 2 * edge + band_h + btn_h + 3 * pad + 2 * inset + (input ? field_gap + field_h : 0);
    const int max_text_h = std::max(line_h, (s(menu_ref_h - 32.0f) - fixed_h) / line_h * line_h);
    const int text_h = std::min(static_cast<int>(lines.size()) * line_h, max_text_h);
    const int body_h = input ? text_h + field_gap + field_h + 2 * inset : std::max(s(72.0f), text_h + 2 * inset);
    const int dialog_h = 2 * edge + band_h + body_h + btn_h + 3 * pad;
    const MenuDialog frame = menu_dialog(ctx, dialog_w, dialog_h, title.c_str(), title_font);
    const UiRect& dialog = frame.frame;
    const UiRect& band = frame.band;
    const UiRect body_box{dialog.x + pad, band.y + band.h, dialog.w - 2 * pad, body_h};
    const int btn_y = body_box.y + body_box.h + pad;

    if (ctx.draw) {
        ui_draw_line_block(UiRect{body_box.x + inset, body_box.y + inset, text_w, text_h}, lines, lay.body_font,
                           line_h, skin::txt_hi.red, skin::txt_hi.green, skin::txt_hi.blue);
        rf::gr::set_color(skin::steel_edge);
        rf::gr::rect(dialog.x + pad, btn_y - pad / 2 - 1, dialog.w - 2 * pad, std::max(1, s(1.0f)));
    }
    lay.field = UiRect{body_box.x + inset, body_box.y + body_h - inset - field_h, text_w, field_h};
    int x = dialog.x + dialog.w - pad - buttons_w;
    for (int i = 0; i < count; ++i) {
        lay.buttons.push_back(UiRect{x, btn_y, btn_w, btn_h});
        x += btn_w + btn_gap;
    }
    return lay;
}

// ─── Stock popups ───
// Stock's popup frame (0x004563B0) runs its key handler through game_poll, then its mouse handler, then its render.
// The last two are replaced; the keys, the default button (popup_current_gadget) and the callbacks stay stock's.

UiContext g_stock_ui;
bool g_stock_ui_ready = false;
// popup_set_text's text whole, which stock keeps only as the three lines it broke it into for its own width.
std::string g_stock_text;

UiId stock_button_id(int index)
{
    return ui_id("stock_popup_button", index);
}

// Returns the button the hit-test pass clicked.
std::optional<int> build_stock_popup(UiContext& ctx)
{
    std::vector<int> shown;
    std::vector<std::string> labels;
    for (int i = 0; i < static_cast<int>(std::size(rf::ui::popup_buttons)); ++i) {
        const rf::ui::Button& button = rf::ui::popup_buttons[i].button;
        if (button.enabled) {
            shown.push_back(i);
            labels.push_back(menu_stock_label(button.text ? button.text : ""));
        }
    }
    std::vector<MenuPopupRowButton> row;
    for (const std::string& label : labels) {
        row.push_back(MenuPopupRowButton{label});
    }
    const bool has_text =
        std::ranges::any_of(rf::ui::popup_text_lines, [](const rf::ui::Label& line) { return line.enabled; });
    const bool input = rf::ui::popup_input_box.enabled;
    const char* title = rf::ui::popup_title.text;
    const MenuPopupLayout lay = menu_popup_layout(ctx, menu_stock_label(title ? title : ""),
                                                  has_text ? std::string_view{g_stock_text} : std::string_view{},
                                                  row, input);

    int& current = rf::ui::popup_current_gadget;
    // Stock's Enter on the input box presses button 0.
    const int lit = current == rf::ui::popup_input_gadget ? 0 : current;
    // As stock's handler: the cursor moving onto a gadget makes it the current one.
    bool moved = false;
    if (!ctx.draw) {
        int dx = 0;
        int dy = 0;
        int dz = 0;
        rf::mouse_get_delta(dx, dy, dz);
        moved = dx != 0 || dy != 0;
    }

    if (input) {
        const auto& typed = rf::ui::popup_input_box.text;
        if (ctx.draw) {
            skin_field_face(ctx, lay.field, std::string_view{typed, strnlen(typed, sizeof(typed))}, lay.body_font,
                            current == rf::ui::popup_input_gadget);
        }
        else if (moved && ui_hover(ctx, lay.field)) {
            current = rf::ui::popup_input_gadget;
        }
    }

    std::optional<int> clicked;
    for (std::size_t k = 0; k < shown.size(); ++k) {
        const int index = shown[k];
        const UiId id = stock_button_id(index);
        if (!ctx.draw && moved && ui_hover(ctx, lay.buttons[k], id)) {
            current = index;
        }
        if (skin_button(ctx, lay.buttons[k], labels[k].c_str(), lay.button_font, SkinButton::Secondary, true, id,
                        index == lit)) {
            clicked = index;
        }
    }
    return clicked;
}

void prepare_stock_ui()
{
    if (!std::exchange(g_stock_ui_ready, true)) {
        menu_ui_reset(g_stock_ui);
    }
}

void stock_popup_set_text(const char* text);
FunHook<decltype(stock_popup_set_text)> stock_popup_set_text_hook{0x00455A50, stock_popup_set_text};
void stock_popup_set_text(const char* text)
{
    g_stock_text = text ? text : "";
    stock_popup_set_text_hook.call_target(text);
}

void stock_popup_process_mouse();
FunHook<decltype(stock_popup_process_mouse)> stock_popup_process_mouse_hook{0x00455B00, stock_popup_process_mouse};
void stock_popup_process_mouse()
{
    // Enter or a hotkey may have closed it this frame.
    if (!rf::ui::popup_is_active()) {
        return;
    }
    prepare_stock_ui();
    std::optional<int> clicked;
    // The popup's frame has polled the input already; a second poll would latch no clicks.
    menu_run_hit_test(g_stock_ui, nullptr, [&](UiContext& ctx) { clicked = build_stock_popup(ctx); });
    if (!clicked) {
        return;
    }
    // Before the click, unlike stock, so a popup its callback opens keeps its own default.
    rf::ui::popup_current_gadget = *clicked;
    ui_play_click();
    // Closes the popup, then runs its callback.
    if (const auto on_click = rf::ui::popup_buttons[*clicked].button.on_click) {
        on_click(g_stock_ui.mx, g_stock_ui.my);
    }
}

void stock_popup_render();
FunHook<decltype(stock_popup_render)> stock_popup_render_hook{0x00455F20, stock_popup_render};
void stock_popup_render()
{
    if (!rf::ui::popup_is_active()) {
        return;
    }
    prepare_stock_ui();
    menu_run_draw(g_stock_ui, [](UiContext& ctx) { build_stock_popup(ctx); }, false);
}

} // namespace

void MenuPopup::open(std::string title, std::string text, std::vector<MenuPopupButton> buttons, int cancel,
                     int default_button, Done done, MenuPopupStyle style)
{
    open_ = true;
    title_ = std::move(title);
    text_ = std::move(text);
    buttons_ = std::move(buttons);
    cancel_ = cancel;
    default_ = default_button;
    style_ = style;
    done_ = std::move(done);
    input_ = false;
    focus_input_ = false;
    pressed_.reset();
    seat_focus_ = true;
}

void MenuPopup::open_input(std::string title, std::string prompt, std::string initial, size_t max_len,
                           UiCharFilter filter, std::string ok_label, Done done)
{
    open(std::move(title), std::move(prompt), {{menu_stock_label(rf::strings::cancel)}, {std::move(ok_label)}}, 0, 1,
         std::move(done), MenuPopupStyle::Neutral);
    input_ = true;
    focus_input_ = true;
    input_max_ = max_len;
    input_filter_ = filter;
    input_text_ = std::move(initial);
    if (input_text_.size() > max_len) {
        input_text_.resize(max_len);
    }
}

void MenuPopup::close()
{
    open_ = false;
    pressed_.reset();
    seat_focus_ = false;
    screen_focus_saved_ = false;
}

int MenuPopup::focused_button(const UiContext& ctx) const
{
    for (int i = 0; i < static_cast<int>(buttons_.size()); ++i) {
        if (ctx.focus_id() == button_id(i)) {
            return i;
        }
    }
    return -1;
}

int MenuPopup::current_button(const UiContext& ctx) const
{
    if (const int focused = focused_button(ctx); focused >= 0) {
        return focused;
    }
    return default_ >= 0 && default_ < static_cast<int>(buttons_.size()) ? default_ : -1;
}

void MenuPopup::seat_focus(UiContext& ctx)
{
    if (!std::exchange(seat_focus_, false)) {
        return;
    }
    // Saved once until the popup closes: open() over a popup still open must not save that popup's button instead.
    if (!screen_focus_saved_) {
        screen_focus_ = ctx.focus_id();
        screen_focus_visible_ = ctx.focus_visible();
        screen_focus_saved_ = true;
    }
    // Over a button id an earlier popup left focused, which would take Enter from the default.
    ctx.set_focus(default_ >= 0 && default_ < static_cast<int>(buttons_.size()) ? button_id(default_) : 0, false);
}

bool MenuPopup::key(UiContext& ctx, int key)
{
    if (!open_) {
        return false;
    }
    seat_focus(ctx);
    const int code = key & rf::KEY_MASK;
    const bool typing = input_ && ctx.focused_text_id() == input_box_id;
    if (code == rf::KEY_ESC) {
        if (cancel_ >= 0) {
            pressed_ = cancel_;
        }
        return true;
    }
    if (code == rf::KEY_ENTER || code == rf::KEY_PADENTER) {
        const int button = typing ? default_ : current_button(ctx);
        if (button >= 0 && button < static_cast<int>(buttons_.size())) {
            pressed_ = button;
        }
        return true;
    }
    if (typing) {
        ctx.push_key(static_cast<rf::Key>(key));
        return true;
    }
    if (key == code) {
        for (int i = 0; i < static_cast<int>(buttons_.size()); ++i) {
            if (buttons_[i].hotkey != rf::KEY_NONE && code == buttons_[i].hotkey) {
                pressed_ = i;
                return true;
            }
        }
    }
    ctx.handle_nav_key(static_cast<rf::Key>(key));
    return true;
}

void MenuPopup::build(UiContext& ctx)
{
    if (!open_) {
        return;
    }
    // The draw pass of the frame it opens in lights the default too.
    seat_focus(ctx);
    const auto secondary = [&](int i) { return style_ == MenuPopupStyle::Neutral || i == cancel_; };
    const int count = static_cast<int>(buttons_.size());
    std::vector<MenuPopupRowButton> row;
    for (int i = 0; i < count; ++i) {
        row.push_back(MenuPopupRowButton{buttons_[i].label, !secondary(i)});
    }
    const MenuPopupLayout lay = menu_popup_layout(ctx, title_, text_, row, input_);

    if (input_) {
        if (!ctx.draw && std::exchange(focus_input_, false)) {
            ctx.focus_text(input_box_id);
        }
        skin_field(ctx, lay.field, input_box_id, input_text_, input_max_, input_filter_, "", lay.body_font, true);
    }

    const int current = current_button(ctx);
    for (int i = 0; i < count; ++i) {
        const SkinButton kind = secondary(i) ? SkinButton::Secondary : SkinButton::Danger;
        if (skin_button(ctx, lay.buttons[i], buttons_[i].label.c_str(), lay.button_font, kind, true, button_id(i),
                        i == current)) {
            pressed_ = i;
        }
    }

    if (!ctx.draw && pressed_) {
        if (input_ && ctx.focused_text_id() == input_box_id) {
            ctx.release_text_focus(*pressed_ != cancel_);
        }
        if (std::exchange(screen_focus_saved_, false)) {
            ctx.set_focus(screen_focus_, screen_focus_visible_);
        }
        open_ = false;
        ui_play_click();
    }
}

void MenuPopup::commit()
{
    if (open_ || !pressed_) {
        return;
    }
    const int button = *pressed_;
    pressed_.reset();
    // `done` may open the next popup over this one's state.
    const Done done = std::exchange(done_, nullptr);
    const std::string text = input_ ? input_text_ : std::string{};
    if (done) {
        done(button, text);
    }
}

void menu_popup_apply_patch()
{
    if (legacy_menus_enabled()) {
        return;
    }
    stock_popup_set_text_hook.install();
    stock_popup_process_mouse_hook.install();
    stock_popup_render_hook.install();
}
