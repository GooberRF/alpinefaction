#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include "ui_kit.h"
#include "../hud/hud_internal.h"
#include "../os/os.h"
#include "../rf/gr/gr_font.h"
#include "../rf/os/frametime.h"
#include "../rf/sound/sound.h"
#include "../sound/sound.h"

namespace
{

constexpr size_t pending_key_max = 64; // each key word queues one entry; the key ring holds 16
constexpr uint32_t fnv_offset_basis = 2166136261u;
constexpr uint32_t fnv_prime = 16777619u;

uint32_t fnv1a(std::string_view text)
{
    uint32_t h = fnv_offset_basis;
    for (const char c : text) {
        h = (h ^ static_cast<uint8_t>(c)) * fnv_prime;
    }
    return h;
}

void apply_color(const rf::Color& c)
{
    rf::gr::set_color(c.red, c.green, c.blue, c.alpha);
}

int font_height(int font)
{
    return rf::gr::get_font_height(font);
}

int text_pad(const UiContext& ctx)
{
    return std::max(ctx.style.text_pad_min, ctx.scaled(ctx.style.text_pad));
}

int scrollbar_width(const UiContext& ctx)
{
    return std::max(ctx.style.scrollbar_w_min, ctx.scaled(ctx.style.scrollbar_w));
}

void ui_play_hover()
{
    rf::snd_play(stock_sound_id::panel_highlight, 0, 0.0f, 1.0f);
}
} // namespace

// ---------------------------------------------------------------------------
// Ids and geometry
// ---------------------------------------------------------------------------

UiId ui_id(std::string_view label)
{
    const uint32_t h = fnv1a(label);
    return h == 0 ? 1u : h;
}

UiId ui_id(std::string_view label, int index)
{
    uint32_t h = fnv1a(label);
    h = (h ^ static_cast<uint32_t>(index)) * fnv_prime;
    return h == 0 ? 1u : h;
}

bool ui_point_in(int px, int py, const UiRect& r)
{
    return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}

bool ui_hover(const UiContext& ctx, const UiRect& r, UiId id)
{
    return ctx.hover(r, id);
}

UiRect ui_draw_rect(const UiContext& ctx, const UiRect& r)
{
    return UiRect{r.x - ctx.origin_x, r.y - ctx.origin_y, r.w, r.h};
}

void ui_set_header_color(const UiContext& ctx)
{
    apply_color(ctx.style.header);
}

void ui_set_text_color(const UiContext& ctx, bool enabled, bool hovered)
{
    if (!enabled) {
        apply_color(ctx.style.text_disabled);
    }
    else if (hovered) {
        apply_color(ctx.style.text_hover);
    }
    else {
        apply_color(ctx.style.text_normal);
    }
}

// ---------------------------------------------------------------------------
// Character filters and formatting
// ---------------------------------------------------------------------------

bool ui_char_filter_filename(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0
        || c == '-' || c == '_' || c == '.' || c == ' ';
}

bool ui_char_filter_numeric(char c)
{
    return std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '.';
}

bool ui_char_filter_digit(char c)
{
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

bool ui_char_filter_hex(char c)
{
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

std::string ui_format_number(float value, const UiNumberFormat& fmt)
{
    if (fmt.zero_label && value == 0.0f) {
        return fmt.zero_label;
    }
    const float shown = value * fmt.display_scale;
    std::string out = fmt.decimals <= 0
        ? std::format("{}", static_cast<int>(std::lround(shown)))
        : std::format("{:.{}f}", shown, fmt.decimals);
    if (fmt.suffix && fmt.suffix[0] != '\0') {
        out += fmt.suffix;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Text measurement / wrapping
// ---------------------------------------------------------------------------

std::string ui_fit_middle(std::string_view text, int max_w, int font)
{
    if (text.empty() || rf::gr::get_string_size(text, font).first <= max_w) {
        return std::string{text};
    }
    for (size_t keep = text.size(); keep > 0; --keep) {
        const size_t left = (keep + 1) / 2;
        const size_t right = keep - left;
        std::string candidate{text.substr(0, left)};
        candidate += "...";
        candidate += std::string{text.substr(text.size() - right, right)};
        if (rf::gr::get_string_size(candidate, font).first <= max_w) {
            return candidate;
        }
    }
    return std::string{"..."};
}

std::vector<std::string> ui_wrap_text(std::string_view text, int max_w, int font)
{
    std::vector<std::string> lines;
    std::string current;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t space = text.find(' ', pos);
        const std::string_view word = text.substr(pos, space == std::string_view::npos ? std::string_view::npos
                                                                                       : space - pos);
        std::string candidate = current.empty() ? std::string{word} : current + " " + std::string{word};
        if (!current.empty() && rf::gr::get_string_size(candidate, font).first > max_w) {
            lines.push_back(current);
            current = std::string{word};
        }
        else {
            current = std::move(candidate);
        }
        if (space == std::string_view::npos) {
            break;
        }
        pos = space + 1;
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

void ui_draw_wrapped(const UiContext& ctx, const UiRect& r, std::string_view text, int font, int line_h)
{
    const int max_lines = std::max(1, r.h / line_h);
    auto lines = ui_wrap_text(text, r.w, font);
    if (static_cast<int>(lines.size()) > max_lines) {
        // Mark the cut so an over-long paragraph never just stops mid-sentence.
        lines.resize(max_lines);
        std::string& last = lines.back();
        while (!last.empty() && rf::gr::get_string_size(last + "...", font).first > r.w) {
            last.pop_back();
        }
        last += "...";
    }

    int y = r.y;
    apply_color(ctx.style.body_text);
    for (const auto& line : lines) {
        rf::gr::string(r.x, y, line.c_str(), font);
        y += line_h;
    }
}

int ui_line_block_height(const std::vector<std::string>& lines, int max_w, int font, int line_h, int max_h)
{
    const int max_lines = std::max(1, max_h / line_h);
    int count = 0;
    for (const auto& line : lines) {
        count += std::max(1, static_cast<int>(ui_wrap_text(line, max_w, font).size()));
        if (count >= max_lines) {
            return max_lines * line_h;
        }
    }
    return count * line_h;
}

void ui_draw_line_block(const UiRect& r, const std::vector<std::string>& lines, int font, int line_h,
                        int red, int green, int blue)
{
    const int max_lines = std::max(1, r.h / line_h);
    std::vector<std::string> out;
    bool truncated = false;
    for (const auto& line : lines) {
        auto wrapped = ui_wrap_text(line, r.w, font);
        if (wrapped.empty()) {
            wrapped.emplace_back(); // a blank line stays a blank line
        }
        for (auto& piece : wrapped) {
            if (static_cast<int>(out.size()) >= max_lines) {
                truncated = true;
                break;
            }
            out.push_back(std::move(piece));
        }
        if (truncated) {
            break;
        }
    }
    if (truncated && !out.empty()) {
        std::string& last = out.back();
        while (!last.empty() && rf::gr::get_string_size(last + "...", font).first > r.w) {
            last.pop_back();
        }
        last += "...";
    }

    int y = r.y;
    rf::gr::set_color(red, green, blue, 255);
    for (const auto& line : out) {
        // ui_wrap_text can only break on spaces, so a single long token stays over-wide.
        rf::gr::string(r.x, y, ui_fit_middle(line, r.w, font).c_str(), font);
        y += line_h;
    }
}

// ---------------------------------------------------------------------------
// Sounds
// ---------------------------------------------------------------------------

void ui_play_click()
{
    rf::snd_play(stock_sound_id::panel_button_click, 0, 0.0f, 1.0f);
}

void ui_play_toggle(bool on)
{
    rf::snd_play(on ? stock_sound_id::checkbox_off : stock_sound_id::checkbox_on, 0, 0.0f, 1.0f);
}

void ui_play_error()
{
    rf::snd_play(stock_sound_id::menu_error, 0, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// UiContext
// ---------------------------------------------------------------------------

void UiContext::begin_frame(int mouse_x, int mouse_y, int wheel_delta, bool lmb_is_down,
                            bool lmb_was_pressed, bool rmb_was_pressed)
{
    mouse_moved_ = mouse_x != prev_mx_ || mouse_y != prev_my_;
    prev_mx_ = mouse_x;
    prev_my_ = mouse_y;
    mx = mouse_x;
    my = mouse_y;
    wheel = wheel_delta;
    lmb_down = lmb_is_down;
    click = lmb_was_pressed;
    rclick = rmb_was_pressed;

    if (!lmb_down) {
        captured_id_ = 0;
    }
    if (mouse_moved_) {
        focus_visible_ = false;
    }
}

void UiContext::set_mouse_pos(int mouse_x, int mouse_y)
{
    mx = mouse_x;
    my = mouse_y;
}

void UiContext::begin_pass(UiPass pass_kind)
{
    rf::gr::reset_clip();
    clip_rect = UiRect{};
    clip_active = false;
    origin_x = 0;
    origin_y = 0;

    draw = pass_kind == UiPass::Draw;
    if (draw) {
        // Edges belong to the hit-test pass; the draw pass never acts on them.
        click = false;
        rclick = false;
        wheel = 0;
        return;
    }

    hovered_id_ = 0;
    focusables_.clear();
    text_seen_this_pass_ = false;
    popup_seen_this_pass_ = false;
    apply_pending_keys();
}

void UiContext::end_pass()
{
    if (draw) {
        return;
    }

    // A pass that never reached the focused box (tab switch, collapsed row, scrolled-away row)
    // commits and drops it, so a blanket input veto cannot persist without a visible box.
    if (text_focus_ != 0 && !text_seen_this_pass_) {
        release_text_focus(true);
    }
    if (popup_id_ != 0 && !popup_seen_this_pass_) {
        close_popup();
    }

    if (hover_sound_ && hovered_id_ != 0 && hovered_id_ != prev_hovered_id_) {
        ui_play_hover();
    }
    prev_hovered_id_ = hovered_id_;

    prev_focusables_.swap(focusables_);
    focusables_.clear();

    // Unconsumed one-shots must not leak into the next frame.
    activate_id_ = 0;
    adjust_id_ = 0;
    adjust_delta_ = 0;
    capture_done_ = false;
    capture_cancelled_ = false;
}

void UiContext::draw_overlays()
{
    if (overlays_.empty()) {
        return;
    }
    rf::gr::reset_clip();
    for (const auto& fn : overlays_) {
        fn();
    }
    overlays_.clear();
}

void UiContext::reset()
{
    release_text_focus(false);
    captured_id_ = 0;
    focus_id_ = 0;
    focus_visible_ = false;
    activate_id_ = 0;
    adjust_id_ = 0;
    adjust_delta_ = 0;
    focusables_.clear();
    prev_focusables_.clear();
    capture_id_ = 0;
    capture_done_ = false;
    capture_cancelled_ = false;
    capture_scan_ = -1;
    capture_mouse_ = -1;
    popup_id_ = 0;
    overlays_.clear();
    scroll_offsets_.clear();
    toasts_.clear();
    hovered_id_ = 0;
    prev_hovered_id_ = 0;
    repeat_id_ = 0;
    repeat_dir_ = 0;
    repeat_count_ = 0;
    repeat_next_ms_ = 0;
    text_changed_ = false;
    description_ = UiDescriptionState{};
}

bool UiContext::hover(const UiRect& r, UiId id) const
{
    // An open dropdown owns input: nothing under it hovers.
    if (popup_id_ != 0 && id != popup_id_) {
        return false;
    }
    if (clip_active && !ui_point_in(mx, my, clip_rect)) {
        return false;
    }
    return ui_point_in(mx, my, r);
}

void UiContext::note_hovered(UiId id)
{
    if (draw || id == 0) {
        return;
    }
    hovered_id_ = id;
    // Hover owns the focus only while the mouse is the live input device, so a keyboard walk is
    // not yanked back by whatever the cursor happens to rest on. The ring stays hidden.
    if (mouse_moved_) {
        focus_id_ = id;
        focus_visible_ = false;
    }
}

void UiContext::register_focusable(UiId id, UiFocusKind kind)
{
    if (draw || id == 0) {
        return;
    }
    focusables_.push_back(Focusable{id, kind});
}

bool UiContext::handle_nav_key(rf::Key key)
{
    const int word = static_cast<int>(key);
    const int code = word & rf::KEY_MASK;
    const bool shifted = (word & rf::KEY_SHIFTED) != 0;

    if (popup_id_ != 0) {
        focus_id_ = popup_id_;
        focus_visible_ = true;
        if (code == rf::KEY_UP || code == rf::KEY_DOWN || code == rf::KEY_LEFT || code == rf::KEY_RIGHT) {
            adjust_id_ = popup_id_;
            adjust_delta_ = code == rf::KEY_UP || code == rf::KEY_LEFT ? -1 : 1;
        }
        else if (code == rf::KEY_ENTER || code == rf::KEY_PADENTER) {
            activate_id_ = popup_id_;
        }
        else if (code == rf::KEY_TAB) {
            close_popup();
        }
        return true;
    }

    if (prev_focusables_.empty()) {
        return false;
    }

    int step = 0;
    if (code == rf::KEY_TAB) {
        step = shifted ? -1 : 1; // 0x100F is the stock Shift+Tab idiom
    }
    else if (code == rf::KEY_DOWN) {
        step = 1;
    }
    else if (code == rf::KEY_UP) {
        step = -1;
    }

    if (step != 0) {
        int index = -1;
        for (size_t i = 0; i < prev_focusables_.size(); ++i) {
            if (prev_focusables_[i].id == focus_id_) {
                index = static_cast<int>(i);
                break;
            }
        }
        const int count = static_cast<int>(prev_focusables_.size());
        index = index < 0 ? (step > 0 ? 0 : count - 1) : (index + step + count) % count;
        focus_id_ = prev_focusables_[index].id;
        focus_visible_ = true;
        return true;
    }

    if (focus_id_ == 0) {
        return false;
    }

    if (code == rf::KEY_ENTER || code == rf::KEY_PADENTER) {
        activate_id_ = focus_id_;
        focus_visible_ = true;
        return true;
    }
    if (code == rf::KEY_LEFT || code == rf::KEY_RIGHT) {
        const auto focused = std::find_if(prev_focusables_.begin(), prev_focusables_.end(),
            [this](const Focusable& f) { return f.id == focus_id_; });
        if (focused == prev_focusables_.end() || focused->kind != UiFocusKind::Adjustable) {
            return false;
        }
        adjust_id_ = focus_id_;
        adjust_delta_ = code == rf::KEY_LEFT ? -1 : 1;
        focus_visible_ = true;
        return true;
    }
    return false;
}

bool UiContext::consume_activation(UiId id)
{
    if (id == 0 || activate_id_ != id) {
        return false;
    }
    activate_id_ = 0;
    return true;
}

int UiContext::consume_adjust(UiId id)
{
    if (id == 0 || adjust_id_ != id) {
        return 0;
    }
    const int delta = adjust_delta_;
    adjust_id_ = 0;
    adjust_delta_ = 0;
    return delta;
}

void UiContext::focus_text(UiId id)
{
    if (id == 0 || text_focus_ == id) {
        return;
    }
    release_text_focus(true);
    text_focus_ = id;
    text_seen_this_pass_ = true;
}

void UiContext::set_text_commit_callback(std::function<void(bool commit)> fn)
{
    text_commit_ = std::move(fn);
}

void UiContext::release_text_focus(bool commit)
{
    if (text_focus_ == 0) {
        return;
    }
    if (text_commit_) {
        // Runs before the buffer is cleared: a numeric box parses out of it.
        text_commit_(commit);
    }
    text_commit_ = nullptr;
    text_focus_ = 0;
    text_buffer_.clear();
    text_target_ = nullptr;
    text_max_len_ = 0;
    text_filter_ = nullptr;
    text_changed_ = false;
    pending_.clear();
}

void UiContext::queue_key(PendingKey key)
{
    if (pending_.size() < pending_key_max) {
        pending_.push_back(std::move(key));
    }
}

void UiContext::push_char(char c)
{
    queue_key(PendingKey{PendingKey::Kind::Char, c});
}

void UiContext::push_key(rf::Key key)
{
    const int code = static_cast<int>(key) & rf::KEY_MASK;
    if (code == rf::KEY_ESC) {
        queue_key(PendingKey{PendingKey::Kind::Escape, 0});
        return;
    }
    if (code == rf::KEY_ENTER || code == rf::KEY_PADENTER) {
        queue_key(PendingKey{PendingKey::Kind::Enter, 0});
        return;
    }
    if (!text_target_) {
        return;
    }
    if (code == rf::KEY_BACKSP) {
        queue_key(PendingKey{PendingKey::Kind::Backspace, 0});
        return;
    }
    if (code == rf::KEY_V && (static_cast<int>(key) & rf::KEY_CTRLED) != 0) {
        const size_t room = text_max_len_ > text_target_->size() ? text_max_len_ - text_target_->size() : 0;
        queue_key(PendingKey{PendingKey::Kind::Paste, 0, read_clipboard_text(room)});
        return;
    }
    const int ascii = rf::key_to_ascii(static_cast<int16_t>(key));
    if (ascii > 0 && ascii < 0x80) {
        push_char(static_cast<char>(ascii));
    }
}

void UiContext::insert_char(char c)
{
    if (text_target_ && text_filter_ && text_filter_(c) && text_target_->size() < text_max_len_) {
        text_target_->push_back(c);
        text_changed_ = true;
    }
}

void UiContext::apply_pending_keys()
{
    if (pending_.empty()) {
        return;
    }
    std::vector<PendingKey> queue;
    queue.swap(pending_);

    for (const PendingKey& pending : queue) {
        if (text_focus_ == 0) {
            break; // an ESC or ENTER earlier in the burst ended the edit
        }
        std::string* const text = text_target_;
        switch (pending.kind) {
            case PendingKey::Kind::Escape:
                release_text_focus(false);
                ui_play_click();
                break;
            case PendingKey::Kind::Enter:
                release_text_focus(true);
                break;
            case PendingKey::Kind::Backspace:
                if (text && !text->empty()) {
                    text->pop_back();
                    text_changed_ = true;
                }
                break;
            case PendingKey::Kind::Char:
                insert_char(pending.c);
                break;
            case PendingKey::Kind::Paste:
                for (const char c : pending.text) {
                    insert_char(c);
                }
                break;
        }
    }
}

void UiContext::begin_key_capture(UiId id)
{
    capture_id_ = id;
    capture_done_ = false;
    capture_cancelled_ = false;
    capture_scan_ = -1;
    capture_mouse_ = -1;
}

void UiContext::cancel_key_capture()
{
    if (capture_id_ == 0) {
        return;
    }
    capture_cancelled_ = true;
}

void UiContext::feed_capture(int scan_code, int mouse_btn)
{
    if (capture_id_ == 0) {
        return;
    }
    capture_scan_ = scan_code;
    capture_mouse_ = mouse_btn;
    capture_done_ = true;
}

bool UiContext::take_key_capture(UiId id, int& scan_code, int& mouse_btn, bool& cancelled)
{
    if (id == 0 || capture_id_ != id) {
        return false;
    }
    if (capture_cancelled_) {
        capture_id_ = 0;
        capture_cancelled_ = false;
        scan_code = -1;
        mouse_btn = -1;
        cancelled = true;
        return true;
    }
    if (!capture_done_) {
        return false;
    }
    capture_id_ = 0;
    capture_done_ = false;
    scan_code = capture_scan_;
    mouse_btn = capture_mouse_;
    cancelled = false;
    return true;
}

void UiContext::open_popup(UiId id)
{
    popup_id_ = id;
    // Opened by its owner during this pass, so the owner was seen.
    if (!draw) {
        popup_seen_this_pass_ = true;
    }
}

void UiContext::close_popup()
{
    popup_id_ = 0;
    if (!draw) {
        click = false;
        rclick = false;
    }
}

bool UiContext::popup_open_for(UiId id)
{
    if (id == 0 || popup_id_ != id) {
        return false;
    }
    if (!draw) {
        popup_seen_this_pass_ = true;
    }
    return true;
}

void UiContext::push_overlay(std::function<void()> fn)
{
    overlays_.push_back(std::move(fn));
}

float& UiContext::scroll_offset(UiId id)
{
    return scroll_offsets_[id];
}

void UiContext::push_toast(std::string text)
{
    toasts_.push_back(Toast{std::move(text), 0.0f});
}

void UiContext::draw_toasts(int font)
{
    if (toasts_.empty()) {
        return;
    }

    const float dt = rf::frametime;
    for (auto& toast : toasts_) {
        toast.age += dt;
    }
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(),
                      [&](const Toast& t) { return t.age >= style.toast_seconds; }),
        toasts_.end());
    if (!draw || toasts_.empty()) {
        return;
    }

    const int pad = std::max(4, scaled(10.0f));
    const int line_h = font_height(font);
    const int h = line_h + 2 * pad;
    const int margin = std::max(6, scaled(16.0f));
    int y = margin;
    for (const auto& toast : toasts_) {
        const int w = rf::gr::get_string_size(toast.text, font).first + 2 * pad;
        const int x = rf::gr::clip_width() - margin - w;
        const float remaining = style.toast_seconds - toast.age;
        const float fade = style.toast_fade_seconds > 0.0f
            ? std::clamp(remaining / style.toast_fade_seconds, 0.0f, 1.0f)
            : 1.0f;
        const auto scale_alpha = [fade](const rf::Color& c) {
            return rf::Color{c.red, c.green, c.blue,
                static_cast<rf::ubyte>(std::lround(static_cast<float>(c.alpha) * fade))};
        };
        apply_color(scale_alpha(style.toast_bg));
        rf::gr::rect(x, y, w, h);
        apply_color(scale_alpha(style.toast_border));
        hud_rect_border(x, y, w, h, 1);
        apply_color(scale_alpha(style.toast_text));
        rf::gr::string(x + pad, y + pad, toast.text.c_str(), font);
        y += h + std::max(3, scaled(6.0f));
    }
}

bool UiContext::repeat_tick(UiId id, int dir)
{
    const int64_t now = timer::get_i64(1000);
    if (repeat_id_ != id || repeat_dir_ != dir) {
        repeat_id_ = id;
        repeat_dir_ = dir;
        repeat_count_ = 0;
        repeat_next_ms_ = now + style.repeat_delay_ms;
        return true;
    }
    if (now >= repeat_next_ms_) {
        repeat_next_ms_ = now + style.repeat_interval_ms;
        ++repeat_count_;
        return true;
    }
    return false;
}

void UiContext::clear_repeat(UiId id)
{
    if (repeat_id_ == id) {
        repeat_id_ = 0;
        repeat_dir_ = 0;
        repeat_count_ = 0;
    }
}

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------

void ui_draw_label(const UiContext& ctx, const UiRect& r, const char* text, int font, bool secondary)
{
    apply_color(secondary ? ctx.style.label_secondary : ctx.style.label);
    const UiRect d = ui_draw_rect(ctx, r);
    rf::gr::string(d.x, d.y, text, font);
}

bool ui_button(UiContext& ctx, const UiRect& r, const char* text, int font, bool enabled, bool active)
{
    const bool hovered = enabled && ui_hover(ctx, r);

    if (ctx.draw) {
        const UiRect d = ui_draw_rect(ctx, r);
        // Active reuses the green the listbox selection and checkbox ticks use.
        if (active) {
            apply_color(hovered ? ctx.style.button_active_fill_hover : ctx.style.button_active_fill);
        }
        else {
            apply_color(hovered ? ctx.style.button_fill_hover : ctx.style.button_fill);
        }
        rf::gr::rect(d.x, d.y, d.w, d.h);
        if (active) {
            apply_color(ctx.style.button_active_border);
        }
        else {
            apply_color(enabled ? ctx.style.button_border : ctx.style.button_border_disabled);
        }
        hud_rect_border(d.x, d.y, d.w, d.h, 1);
        if (active) {
            apply_color(ctx.style.button_active_text);
        }
        else {
            ui_set_text_color(ctx, enabled, hovered);
        }
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, d.x + d.w / 2,
            d.y + (d.h - font_height(font)) / 2, text, font);
        return false;
    }

    return hovered && ctx.click;
}

bool ui_checkbox(UiContext& ctx, const UiRect& r, bool checked, const char* text, int font, bool enabled)
{
    const bool hovered = enabled && ui_hover(ctx, r);

    if (ctx.draw) {
        const UiRect d = ui_draw_rect(ctx, r);
        const int box = std::min(d.h - 2, font_height(font));
        const int by = d.y + (d.h - box) / 2;
        apply_color(hovered ? ctx.style.button_fill_hover : ctx.style.button_fill);
        rf::gr::rect(d.x, by, box, box);
        apply_color(enabled ? ctx.style.check_border : ctx.style.check_border_disabled);
        hud_rect_border(d.x, by, box, box, 1);
        if (checked) {
            // Muted tick for a pinned/non-interactive checkbox.
            apply_color(enabled ? ctx.style.check_mark : ctx.style.check_mark_disabled);
            rf::gr::rect(d.x + 2, by + 2, box - 4, box - 4);
        }
        ui_set_text_color(ctx, enabled, hovered);
        rf::gr::string(d.x + box + std::max(ctx.style.check_text_gap_min, ctx.scaled(ctx.style.check_text_gap)),
            d.y + (d.h - font_height(font)) / 2, text, font);
        return false;
    }

    return hovered && ctx.click;
}

int ui_cycler_arrow_width(const UiContext& ctx)
{
    return std::max(ctx.style.cycler_arrow_w_min, ctx.scaled(ctx.style.cycler_arrow_w));
}

int ui_cycler_value_width(const UiContext& ctx, const UiRect& r)
{
    return std::max(0, r.w - 2 * ui_cycler_arrow_width(ctx));
}

int ui_cycler(UiContext& ctx, const UiRect& r, const char* value, int font)
{
    const int arrow_w = ui_cycler_arrow_width(ctx);
    const UiRect left{r.x, r.y, arrow_w, r.h};
    const UiRect right{r.x + r.w - arrow_w, r.y, arrow_w, r.h};
    const UiRect middle{r.x + arrow_w, r.y, r.w - 2 * arrow_w, r.h};

    const bool hover_left = ui_hover(ctx, left);
    const bool hover_right = ui_hover(ctx, right);
    const bool hover_mid = ui_hover(ctx, middle);

    if (ctx.draw) {
        const UiRect d = ui_draw_rect(ctx, r);
        const UiRect dl = ui_draw_rect(ctx, left);
        const UiRect dr = ui_draw_rect(ctx, right);
        const UiRect dm = ui_draw_rect(ctx, middle);

        apply_color(hover_mid ? ctx.style.button_fill_hover : ctx.style.button_fill);
        rf::gr::rect(d.x, d.y, d.w, d.h);
        apply_color(ctx.style.cycler_border);
        hud_rect_border(d.x, d.y, d.w, d.h, 1);

        const int text_y = d.y + (d.h - font_height(font)) / 2;
        ui_set_text_color(ctx, true, hover_left);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, dl.x + dl.w / 2, text_y, "<", font);
        ui_set_text_color(ctx, true, hover_right);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, dr.x + dr.w / 2, text_y, ">", font);

        ui_set_text_color(ctx, true, hover_mid);
        if (ctx.clip_active) {
            // Already inside a scroll region's clip window, and set_clip replaces rather than
            // nests, so rely on that clip plus the caller's fit_middle() sizing.
            rf::gr::string_aligned(rf::gr::ALIGN_CENTER, dm.x + dm.w / 2,
                dm.y + (dm.h - font_height(font)) / 2, value, font);
        }
        else {
            int save_x = 0, save_y = 0, save_w = 0, save_h = 0;
            rf::gr::get_clip(&save_x, &save_y, &save_w, &save_h);
            rf::gr::set_clip(dm.x, dm.y, dm.w, dm.h);
            // set_clip moved the drawing origin, so draw relative to the clip window.
            rf::gr::string_aligned(rf::gr::ALIGN_CENTER, dm.w / 2,
                (dm.h - font_height(font)) / 2, value, font);
            rf::gr::set_clip(save_x, save_y, save_w, save_h);
        }
        return 0;
    }

    // Right-click anywhere on the widget steps backwards, so a value can be walked either way
    // without aiming for the small left arrow.
    if (ctx.rclick && (hover_left || hover_right || hover_mid)) {
        return -1;
    }
    if (!ctx.click) {
        return 0;
    }
    if (hover_left) {
        return -1;
    }
    if (hover_right || hover_mid) {
        return 1;
    }
    return 0;
}

int ui_listbox_row_height(int font)
{
    return font_height(font) + 2;
}

int ui_listbox(UiContext& ctx, const UiRect& r, const std::vector<std::string>& items, int sel,
               float& scroll, int font)
{
    const int row_h = ui_listbox_row_height(font);
    const int total_h = static_cast<int>(items.size()) * row_h;
    const float max_scroll = static_cast<float>(std::max(0, total_h - r.h));
    const bool inside = ui_hover(ctx, r);

    if (!ctx.draw && inside && ctx.wheel != 0) {
        scroll += (ctx.wheel > 0 ? -1.0f : 1.0f) * static_cast<float>(row_h * 3);
    }
    scroll = std::clamp(scroll, 0.0f, max_scroll);

    if (!ctx.draw) {
        if (inside && ctx.click) {
            const int index = (ctx.my - r.y + static_cast<int>(scroll)) / row_h;
            if (index >= 0 && index < static_cast<int>(items.size())) {
                return index;
            }
        }
        return -1;
    }

    const UiRect d = ui_draw_rect(ctx, r);
    apply_color(ctx.style.list_bg);
    rf::gr::rect(d.x, d.y, d.w, d.h);
    apply_color(ctx.style.list_border);
    hud_rect_border(d.x, d.y, d.w, d.h, 1);

    int save_x = 0, save_y = 0, save_w = 0, save_h = 0;
    rf::gr::get_clip(&save_x, &save_y, &save_w, &save_h);
    // set_clip takes absolute screen coordinates and replaces the window entirely.
    rf::gr::set_clip(r.x, r.y, r.w, r.h);
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const int iy = r.y + i * row_h - static_cast<int>(scroll);
        if (iy + row_h < r.y || iy > r.y + r.h) {
            continue;
        }
        // Drawing coords are relative to the clip window origin.
        const int dy = iy - r.y;
        const bool hovered = inside && ctx.my >= iy && ctx.my < iy + row_h;
        if (i == sel) {
            apply_color(ctx.style.list_selected_bg);
            rf::gr::rect(0, dy, r.w, row_h);
        }
        else if (hovered) {
            apply_color(ctx.style.list_hover_bg);
            rf::gr::rect(0, dy, r.w, row_h);
        }
        if (i == sel) {
            apply_color(ctx.style.list_selected_text);
        }
        else {
            apply_color(hovered ? ctx.style.text_hover : ctx.style.text_normal);
        }
        rf::gr::string(3, dy + 1, items[i].c_str(), font);
    }
    rf::gr::set_clip(save_x, save_y, save_w, save_h);

    if (max_scroll > 0.0f) {
        const float ratio = scroll / max_scroll;
        const int bar_w = scrollbar_width(ctx);
        const int bar_h = std::max(bar_w, r.h * r.h / std::max(1, total_h));
        const int bar_y = d.y + static_cast<int>(ratio * (r.h - bar_h));
        apply_color(ctx.style.scrollbar);
        rf::gr::rect(d.x + r.w - bar_w, bar_y, bar_w, bar_h);
    }
    return -1;
}

UiTextInputResult ui_text_input(UiContext& ctx, const UiRect& r, UiId id, std::string& text,
                                size_t max_len, UiCharFilter allow_char, const char* hint, int font,
                                bool enabled)
{
    UiTextInputResult res;
    const bool focused_at_entry = ctx.text_focus_ == id;

    if (!ctx.draw) {
        // Focus on a greyed box strands the input veto with nothing to type into.
        if (focused_at_entry && !enabled) {
            ctx.release_text_focus(false);
            return res;
        }
        if (focused_at_entry) {
            ctx.text_seen_this_pass_ = true;
        }
        if (!focused_at_entry && enabled && ctx.consume_activation(id)) {
            ctx.focus_text(id);
            res.gained_focus = true;
        }
        if (ctx.click) {
            const bool inside = enabled && ui_hover(ctx, r, id);
            if (inside && !focused_at_entry) {
                ctx.release_text_focus(true); // commit whatever box held it
                ctx.text_focus_ = id;
                // Latch on gain as well as on hold.
                ctx.text_seen_this_pass_ = true;
                res.gained_focus = true;
            }
            else if (!inside && focused_at_entry) {
                ctx.release_text_focus(true);
            }
        }
        if (focused_at_entry && ctx.text_focus_ == id) {
            ctx.text_target_ = &text;
            ctx.text_max_len_ = max_len;
            ctx.text_filter_ = allow_char;
            res.changed = ctx.text_changed_;
            ctx.text_changed_ = false;
        }
        // Deliberately NOT bound on the focusing pass: callers pass a temporary on that pass and
        // only bind the persistent buffer once `focused` is true.
        return res;
    }

    // Draw pass. Layout is absolute screen space; inside a scroll region the clip origin has moved
    // the drawing origin, so subtract it like every other widget does.
    const UiRect d = ui_draw_rect(ctx, r);
    const bool focused = ctx.text_focus_ == id;
    apply_color(ctx.style.input_bg);
    rf::gr::rect(d.x, d.y, d.w, d.h);
    apply_color(!enabled ? ctx.style.input_border_disabled
                       : (focused ? ctx.style.input_border_focused : ctx.style.input_border));
    hud_rect_border(d.x, d.y, d.w, d.h, 1);
    const int pad = text_pad(ctx);
    const int text_y = d.y + (d.h - font_height(font)) / 2;
    if (text.empty() && !focused) {
        apply_color(enabled ? ctx.style.input_hint : ctx.style.input_hint_disabled);
        rf::gr::string(d.x + pad, text_y, hint ? hint : "", font);
    }
    else {
        // Trailing underscore as the caret while typing.
        const std::string shown = focused ? text + "_" : text;
        apply_color(enabled ? ctx.style.input_text : ctx.style.input_text_disabled);
        rf::gr::string(d.x + pad, text_y,
            hud_fit_string(shown, d.w - 2 * pad, nullptr, font).c_str(), font);
    }
    return res;
}

bool ui_close_button(UiContext& ctx, const UiRect& r, int font)
{
    const bool hovered = ui_hover(ctx, r);

    if (ctx.draw) {
        const UiRect d = ui_draw_rect(ctx, r);
        apply_color(hovered ? ctx.style.danger_fill : ctx.style.button_fill);
        rf::gr::rect(d.x, d.y, d.w, d.h);
        apply_color(hovered ? ctx.style.danger_border : ctx.style.button_border);
        hud_rect_border(d.x, d.y, d.w, d.h, 1);
        ui_set_text_color(ctx, true, hovered);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, d.x + d.w / 2,
            d.y + (d.h - font_height(font)) / 2, "X", font);
        return false;
    }

    return hovered && ctx.click;
}

// ---------------------------------------------------------------------------
// Scroll regions
// ---------------------------------------------------------------------------

UiScrollRegion ui_scroll_begin(UiContext& ctx, const UiRect& view, int content_h, float& scroll,
                               int wheel_step, UiId id, bool draggable)
{
    UiScrollRegion region;
    region.view = view;
    region.scroll = &scroll;
    region.content_h = content_h;
    region.id = id;
    region.draggable = draggable;
    region.wheel_step = wheel_step;
    region.max_scroll = static_cast<float>(std::max(0, content_h - view.h));

    // An open popup owns the wheel, and scrolling would carry its owner away from under it.
    if (!ctx.draw && region.max_scroll > 0.0f && ctx.wheel != 0 && ctx.popup_id() == 0
        && ui_point_in(ctx.mx, ctx.my, view)) {
        scroll += (ctx.wheel > 0 ? -1.0f : 1.0f) * static_cast<float>(wheel_step);
    }
    // Also clamps after the content shrinks.
    scroll = std::clamp(scroll, 0.0f, region.max_scroll);

    // Gate hit-testing on the viewport in both passes so rows scrolled out of sight can never be
    // clicked.
    region.saved_clip_rect = ctx.clip_rect;
    region.saved_clip_active = ctx.clip_active;
    region.saved_origin_x = ctx.origin_x;
    region.saved_origin_y = ctx.origin_y;
    ctx.clip_rect = view;
    ctx.clip_active = true;

    if (ctx.draw) {
        rf::gr::get_clip(&region.saved_gr_x, &region.saved_gr_y, &region.saved_gr_w, &region.saved_gr_h);
        rf::gr::set_clip(view.x, view.y, view.w, view.h);
        // set_clip moves the drawing origin; widgets subtract it via ui_draw_rect.
        ctx.origin_x = view.x;
        ctx.origin_y = view.y;
    }
    return region;
}

void ui_scroll_end(UiContext& ctx, UiScrollRegion& region)
{
    if (ctx.draw) {
        rf::gr::set_clip(region.saved_gr_x, region.saved_gr_y, region.saved_gr_w, region.saved_gr_h);
    }

    ctx.clip_rect = region.saved_clip_rect;
    ctx.clip_active = region.saved_clip_active;
    ctx.origin_x = region.saved_origin_x;
    ctx.origin_y = region.saved_origin_y;

    if (region.max_scroll <= 0.0f) {
        return;
    }

    // Scrollbar, drawn after the clip is restored so it uses absolute coords.
    const UiScrollbarGeometry bar = ui_scrollbar_geometry(ctx, region.view, region.content_h, *region.scroll);

    if (!ctx.draw && region.draggable && region.id != 0) {
        // Where on the thumb the drag took hold, so grabbing it never makes it jump.
        constexpr UiId grab_salt = 0x6AB5C0F1u;
        float& grab = ctx.scroll_offset(region.id ^ grab_salt);
        if (ctx.click && ui_point_in(ctx.mx, ctx.my, bar.gutter)) {
            if (ctx.my < bar.track.y || ctx.my >= bar.track.y + bar.track.h) {
                const float step = static_cast<float>(region.wheel_step);
                *region.scroll = std::clamp(*region.scroll + (ctx.my < bar.track.y ? -step : step), 0.0f,
                                            region.max_scroll);
            }
            else {
                ctx.capture(region.id);
                const bool on_thumb = ctx.my >= bar.thumb_y && ctx.my < bar.thumb_y + bar.thumb_h;
                grab = static_cast<float>(on_thumb ? ctx.my - bar.thumb_y : bar.thumb_h / 2);
            }
        }
        if (ctx.captured_id() == region.id) {
            if (ctx.lmb_down) {
                const int travel = std::max(1, bar.track.h - bar.thumb_h);
                const float t = std::clamp(
                    (static_cast<float>(ctx.my - bar.track.y) - grab) / static_cast<float>(travel), 0.0f, 1.0f);
                *region.scroll = t * region.max_scroll;
            }
            else {
                ctx.release_capture();
            }
        }
    }

    if (!ctx.draw) {
        return;
    }
    if (ctx.style.scrollbar_track.alpha != 0) {
        apply_color(ctx.style.scrollbar_track);
        rf::gr::rect(bar.gutter.x, bar.gutter.y, bar.gutter.w, bar.gutter.h);
    }
    const bool dragging = region.draggable && region.id != 0 && ctx.captured_id() == region.id;
    const rf::Color& thumb = dragging ? ctx.style.scrollbar_hover : ctx.style.scrollbar;
    if (thumb.alpha != 0) {
        apply_color(thumb);
        rf::gr::rect(bar.gutter.x, bar.thumb_y, bar.gutter.w, bar.thumb_h);
    }
}

UiScrollbarGeometry ui_scrollbar_geometry(const UiContext& ctx, const UiRect& view, int content_h, float scroll)
{
    UiScrollbarGeometry g;
    const int bar_w = scrollbar_width(ctx);
    const int cap = std::min(ctx.scaled(ctx.style.scrollbar_cap), view.h / 4);
    g.gutter = UiRect{view.x + view.w - bar_w, view.y, bar_w, view.h};
    g.track = UiRect{g.gutter.x, view.y + cap, bar_w, std::max(0, view.h - 2 * cap)};
    g.thumb_h = std::min(g.track.h, std::max(bar_w, g.track.h * view.h / std::max(1, content_h)));
    const int max_scroll = std::max(0, content_h - view.h);
    const float ratio = max_scroll > 0 ? std::clamp(scroll / static_cast<float>(max_scroll), 0.0f, 1.0f) : 0.0f;
    g.thumb_y = g.track.y + static_cast<int>(ratio * static_cast<float>(g.track.h - g.thumb_h));
    return g;
}
