#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "message_log_menu.h"
#include "menu_shell.h"
#include "settings_skin.h"
#include "ui_kit.h"
#include "../rf/bmpman.h"
#include "../rf/gameseq.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/hud.h"
#include "../rf/input.h"
#include "../rf/localize.h"
#include "../rf/player/control_config.h"
#include "../rf/player/player.h"

namespace
{

// Stock scrolls a message at a time, again every 200 ms while a key is held.
constexpr std::chrono::milliseconds repeat_time{200};
const UiId entries_id = ui_id("message_log_entries");
const UiId close_id = ui_id("message_log_close");
// The speaker's portrait right of the name, and the gap between them.
constexpr float portrait_size = 56.0f;
constexpr float portrait_gap = 10.0f;

struct Entry
{
    std::string name;
    int portrait = -1;
    std::vector<std::string> lines;
    // From the top of the entries.
    int y = 0;
    int h = 0;
};

// What g_entries was wrapped for: each frame builds twice, and wrapping 80 messages is not free.
struct MeasureKey
{
    int count = 0;
    int latest = 0;
    int text_w = 0;
    int name_w = 0;
    int font = 0;
    int name_font = 0;
    int line_h = 0;
    int portrait_h = 0;
    int portrait_name_w = 0;

    bool operator==(const MeasureKey&) const = default;
};

UiContext g_ui;
std::vector<Entry> g_entries;
std::optional<MeasureKey> g_measured;
float g_scroll = 0.0f;
float g_max_scroll = 0.0f;
std::chrono::steady_clock::time_point g_next_step{};
bool g_close = false;
bool g_closing = false;

rf::HudPersonaInfo* persona_info(int persona)
{
    const int count = std::min(rf::num_hud_personas, static_cast<int>(std::size(rf::hud_personas_info)));
    return persona >= 0 && persona < count ? &rf::hud_personas_info[persona] : nullptr;
}

std::string persona_name(const rf::HudPersonaInfo& info)
{
    return info.display_name.empty() ? std::string{info.name.c_str()} : std::string{info.display_name.c_str()};
}

// Loaded as stock's persona message loads it the first time the persona speaks, which a loaded save's log may predate.
int persona_portrait(rf::HudPersonaInfo& info)
{
    if (info.image_handle == -1 && !info.image.empty()) {
        info.image_handle = rf::bm::load(info.image.c_str(), -1, false);
    }
    return info.image_handle;
}

// Newest first. The log keeps each message broken for the HUD, so it is wrapped again to the column here; each
// entry takes a blank line after it, as stock's do, and is at least as tall as its portrait. Beside a portrait the name
// has `portrait_name_w`.
void measure(int text_w, int name_w, int font, int name_font, int line_h, int portrait_h, int portrait_name_w)
{
    const int ring = static_cast<int>(std::size(rf::logged_messages));
    const int count = std::clamp(rf::num_logged_messages, 0, ring);
    const MeasureKey key{
        count, rf::latest_logged_message, text_w, name_w, font, name_font, line_h, portrait_h, portrait_name_w};
    if (g_measured == key) {
        return;
    }
    g_measured = key;
    g_entries.clear();
    int y = 0;
    for (int i = 0; i < count; ++i) {
        int slot = rf::latest_logged_message - i;
        if (slot < 0) {
            slot += ring;
        }
        if (slot < 0 || slot >= ring) {
            break;
        }
        const rf::LoggedMessage& msg = rf::logged_messages[slot];
        std::string text{msg.text, strnlen(msg.text, sizeof(msg.text))};
        std::ranges::replace(text, '\n', ' ');
        std::ranges::replace(text, '\r', ' ');
        Entry entry;
        if (rf::HudPersonaInfo* info = persona_info(msg.persona)) {
            entry.portrait = persona_portrait(*info);
            entry.name = ui_fit_end(persona_name(*info), entry.portrait >= 0 ? portrait_name_w : name_w, name_font);
        }
        entry.lines = ui_wrap_text(text, text_w, font);
        entry.y = y;
        const int lines_h = std::max(1, static_cast<int>(entry.lines.size())) * line_h;
        entry.h = (entry.portrait >= 0 ? std::max(lines_h, portrait_h) : lines_h) + line_h;
        y += entry.h;
        g_entries.push_back(std::move(entry));
    }
}

// To the start of the message before or after the one at the top.
void step(int dir)
{
    if (dir < 0) {
        float target = 0.0f;
        for (const Entry& entry : g_entries) {
            if (static_cast<float>(entry.y) >= g_scroll - 0.5f) {
                break;
            }
            target = static_cast<float>(entry.y);
        }
        g_scroll = target;
        return;
    }
    for (const Entry& entry : g_entries) {
        if (static_cast<float>(entry.y) > g_scroll + 0.5f) {
            g_scroll = std::min(static_cast<float>(entry.y), g_max_scroll);
            return;
        }
    }
    g_scroll = g_max_scroll;
}

// Stock's scroll keys: Down or Backward, else Up or Forward.
int held_direction(rf::ControlConfig* cc)
{
    const auto control = [cc](rf::ControlConfigAction action) {
        return cc && rf::control_config_check_pressed(cc, action, nullptr);
    };
    if (rf::key_is_down(rf::KEY_DOWN) || control(rf::CC_ACTION_BACKWARD)) {
        return 1;
    }
    if (rf::key_is_down(rf::KEY_UP) || control(rf::CC_ACTION_FORWARD)) {
        return -1;
    }
    return 0;
}

void build(UiContext& ctx)
{
    const float scale = menu_screen_scale();
    ctx.set_scale(scale);
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const int title_font = menu_font(true, 24.0f, scale);
    const int font = menu_font(false, 20.0f, scale);
    const int name_font = menu_font(true, 20.0f, scale);
    const int line_h = rf::gr::get_font_height(font) + s(4.0f);

    const MenuDialog dialog =
        menu_dialog(ctx, s(1100.0f), s(820.0f), menu_stock_label(rf::strings::messages).c_str(), title_font);
    const int pad = s(14.0f);
    const int btn_h = s(44.0f);
    const int btn_y = dialog.frame.y + dialog.frame.h - pad - btn_h;
    const int well_y = dialog.band.y + dialog.band.h + pad;
    const UiRect well{dialog.frame.x + pad, well_y, dialog.frame.w - 2 * pad, btn_y - pad - well_y};
    const UiRect view{well.x + s(4.0f), well.y + s(4.0f), well.w - s(8.0f), well.h - s(8.0f)};
    const int inner_w = view.w - ui_scrollbar_width(ctx) - s(4.0f);
    const int name_x = s(12.0f);
    const int name_w = s(190.0f);
    const int text_x = name_x + name_w + s(16.0f);
    const int text_w = std::max(1, inner_w - text_x - s(12.0f));
    const int portrait_h = s(portrait_size);
    // Against the column's right end, so the portraits line up.
    const int portrait_x = name_x + name_w - portrait_h;

    measure(text_w, name_w, font, name_font, line_h, portrait_h, std::max(1, portrait_x - s(portrait_gap) - name_x));
    const int content_h = g_entries.empty() ? 0 : g_entries.back().y + g_entries.back().h;
    g_max_scroll = static_cast<float>(std::max(0, content_h - view.h));

    if (!ctx.draw) {
        rf::ControlConfig* cc = rf::local_player ? &rf::local_player->settings.controls : nullptr;
        // Held, as stock reads them: the game's own poll beneath has taken the key events.
        if (rf::key_is_down(rf::KEY_ESC)
            || (cc && rf::control_config_check_pressed(cc, rf::CC_ACTION_MESSAGES, nullptr))) {
            g_close = true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (const int dir = held_direction(cc); dir != 0 && now >= g_next_step) {
            step(dir);
            g_next_step = now + repeat_time;
        }
        if (ctx.wheel != 0 && ui_point_in(ctx.mx, ctx.my, dialog.frame)) {
            step(ctx.wheel > 0 ? -1 : 1);
        }
        // The bar's end boxes step a message too; the region only drags the thumb.
        const UiScrollbarGeometry bar = ui_scrollbar_geometry(ctx, view, content_h, g_scroll);
        if (g_max_scroll > 0.0f && ctx.click && ui_point_in(ctx.mx, ctx.my, bar.gutter)
            && (ctx.my < bar.track.y || ctx.my >= bar.track.y + bar.track.h)) {
            step(ctx.my < bar.track.y ? -1 : 1);
        }
    }
    else {
        skin_slot(ctx, well.x, well.y, well.w, well.h);
    }

    UiScrollRegion region = ui_scroll_begin(ctx, view, content_h, g_scroll, 0, entries_id, true);
    if (ctx.draw) {
        if (g_entries.empty()) {
            const UiRect d = ui_draw_rect(ctx, view);
            rf::gr::set_color(0, 0xC0, 0, 0xFF);
            rf::gr::string_aligned(rf::gr::ALIGN_CENTER, d.x + d.w / 2, d.y + (d.h - rf::gr::get_font_height(font)) / 2,
                                   rf::strings::no_messages, font);
        }
        for (std::size_t i = 0; i < g_entries.size(); ++i) {
            const Entry& entry = g_entries[i];
            const int y = region.start_y() + entry.y;
            if (y + entry.h < view.y || y > view.y + view.h) {
                continue;
            }
            const UiRect d = ui_draw_rect(ctx, UiRect{view.x, y, inner_w, entry.h});
            if (i > 0) {
                rf::gr::set_color(skin::border_dim);
                rf::gr::rect(d.x + name_x, d.y, d.w - 2 * name_x, std::max(1, s(1.0f)));
            }
            const int top = d.y + line_h / 2;
            if (!entry.name.empty()) {
                skin_draw_text(d.x + name_x, top, entry.name, name_font, rf::hud_msg_color);
            }
            if (entry.portrait >= 0) {
                menu_draw_portrait(ctx, UiRect{d.x + portrait_x, top, portrait_h, portrait_h}, entry.portrait);
            }
            int line_y = top;
            for (const std::string& line : entry.lines) {
                skin_draw_text(d.x + text_x, line_y, line, font, rf::hud_default_color);
                line_y += line_h;
            }
        }
    }
    ui_scroll_end(ctx, region);
    if (ctx.draw) {
        skin_scrollbar(ctx, view, content_h, g_scroll, ctx.captured_id() == entries_id);
    }

    const std::string close_label = menu_stock_label(rf::strings::close);
    const int btn_w = skin_button_width(ctx, close_label.c_str(), name_font, 150.0f);
    const UiRect close_rect{dialog.frame.x + dialog.frame.w - pad - btn_w, btn_y, btn_w, btn_h};
    if (skin_button(ctx, close_rect, close_label.c_str(), name_font, SkinButton::Secondary, true, close_id)) {
        g_close = true;
    }
}

} // namespace

void message_log_menu_enter()
{
    menu_ui_reset(g_ui);
    g_measured.reset();
    g_scroll = 0.0f;
    g_close = false;
    g_closing = false;
    g_next_step = std::chrono::steady_clock::now() + repeat_time;
}

void message_log_menu_do_frame(bool paused)
{
    // paused = 1 means a stock popup owns input this frame; it renders after us and on top.
    if (!paused) {
        // The game beneath has polled the keys and the mouse this frame, as stock's log relies on; a second poll would
        // latch no click. The pause menu its Esc pushes gives way to this pop, as under stock's log.
        menu_run_hit_test(g_ui, nullptr, build);
        if (g_close && !g_closing) {
            g_closing = true;
            ui_play_click();
            rf::gameseq_pop_state();
        }
    }
    // A transparent state: the game beneath has drawn its frame.
    menu_run_draw(g_ui, build, false);
}
