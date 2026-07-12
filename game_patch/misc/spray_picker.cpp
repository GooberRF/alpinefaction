#include <algorithm>
#include <cmath>
#include <string>
#include "spray_picker.h"
#include "alpine_settings.h"
#include "../multi/sprays.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/bmpman.h"
#include "../rf/input.h"
#include "../rf/ui.h"
#include "../rf/gameseq.h"
#include "../hud/hud.h"
#include "../hud/hud_internal.h"

namespace
{
    bool g_open = false;
    float g_scroll = 0.0f;
    int g_cursor_x = 0;
    int g_cursor_y = 0;

    constexpr float REF_WIDTH = 1280.0f;
    constexpr float REF_HEIGHT = 800.0f;
    constexpr float TARGET_THUMB = 160.0f;
    constexpr int MIN_COLS = 2;
    constexpr int MAX_COLS = 6;

    struct Layout
    {
        float scale;
        int px, py, pw, ph;                             // panel rect
        int content_x, content_y, content_w, content_h; // scrollable grid region (clip window)
        int gap;                                        // spacing between cells / edges
        int thumb;                                      // square thumbnail size
        int cols;
        int count;
        int total_grid_h;                               // full (unclipped) height of all rows
        int cancel_x, cancel_y, cancel_w, cancel_h;     // Cancel button rect
    };

    Layout compute_layout()
    {
        Layout lo{};
        const int clip_w = rf::gr::clip_width();
        const int clip_h = rf::gr::clip_height();
        lo.scale = std::min(clip_w / REF_WIDTH, clip_h / REF_HEIGHT);

        const auto s = [&](float v) { return static_cast<int>(v * lo.scale); };

        // Compact panel that leaves a clear margin at every resolution.
        lo.pw = std::min(s(880.0f), clip_w - 80);
        lo.ph = std::min(s(600.0f), clip_h - 80);
        lo.px = (clip_w - lo.pw) / 2;
        lo.py = (clip_h - lo.ph) / 2;

        const int pad = s(16.0f);
        const int title_h = s(46.0f);
        const int footer_h = s(58.0f);

        lo.gap = std::max(4, s(14.0f));

        lo.content_x = lo.px + pad;
        lo.content_y = lo.py + title_h;
        lo.content_w = lo.pw - 2 * pad;
        lo.content_h = lo.ph - title_h - footer_h;

        // Derive column count from width and the target thumbnail size, then clamp.
        const int target = std::max(1, s(TARGET_THUMB));
        lo.cols = std::clamp((lo.content_w - lo.gap) / (target + lo.gap), MIN_COLS, MAX_COLS);
        lo.thumb = (lo.content_w - (lo.cols + 1) * lo.gap) / lo.cols;
        if (lo.thumb < 1) {
            lo.thumb = 1;
        }

        lo.count = spray_count();
        const int rows = (lo.count + lo.cols - 1) / lo.cols;
        lo.total_grid_h = rows * (lo.thumb + lo.gap) + lo.gap;

        lo.cancel_w = std::min(s(240.0f), lo.content_w);
        lo.cancel_h = s(40.0f);
        lo.cancel_x = lo.px + (lo.pw - lo.cancel_w) / 2;
        lo.cancel_y = lo.py + lo.ph - footer_h + (footer_h - lo.cancel_h) / 2;

        return lo;
    }

    // Screen rect of cell `i`, accounting for the current scroll offset.
    void cell_rect(const Layout& lo, int i, int& out_x, int& out_y)
    {
        const int col = i % lo.cols;
        const int row = i / lo.cols;
        out_x = lo.content_x + lo.gap + col * (lo.thumb + lo.gap);
        out_y = lo.content_y + lo.gap + row * (lo.thumb + lo.gap) - static_cast<int>(g_scroll);
    }

    float max_scroll(const Layout& lo)
    {
        return std::max(0.0f, static_cast<float>(lo.total_grid_h - lo.content_h));
    }

    bool point_in(int px, int py, int x, int y, int w, int h)
    {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
}

void spray_picker_open()
{
    g_open = true;
    g_scroll = 0.0f;
}

void spray_picker_close()
{
    g_open = false;
    g_scroll = 0.0f;
}

bool spray_picker_is_open()
{
    return g_open;
}

void spray_picker_render()
{
    if (!g_open) {
        return;
    }

    // Close the picker if our game state changes (i.e. level change, disconnect, etc.)
    // so the picker doesn't get stuck open.
    if (rf::gameseq_get_state() != rf::GS_OPTIONS_MENU) {
        spray_picker_close();
        return;
    }

    const Layout lo = compute_layout();
    g_scroll = std::clamp(g_scroll, 0.0f, max_scroll(lo));

    const int font = rf::ui::medium_font_0;

    // Use the stored menu cursor (screen/clip space) for hover feedback so it matches hit-testing.
    const int mx = g_cursor_x;
    const int my = g_cursor_y;
    const bool cursor_in_content = point_in(mx, my, lo.content_x, lo.content_y, lo.content_w, lo.content_h);

    // Full-screen dim.
    rf::gr::set_color(0, 0, 0, 192);
    rf::gr::rect(0, 0, rf::gr::clip_width(), rf::gr::clip_height());

    // Panel background + border.
    rf::gr::set_color(20, 20, 20, 235);
    rf::gr::rect(lo.px, lo.py, lo.pw, lo.ph);
    rf::gr::set_color(120, 120, 120, 255);
    hud_rect_border(lo.px, lo.py, lo.pw, lo.ph, std::max(1, static_cast<int>(2 * lo.scale)));

    // Title.
    rf::gr::set_color(255, 255, 255, 255);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, lo.px + lo.pw / 2,
        lo.py + static_cast<int>(14 * lo.scale), "Select Spray", font);

    // Grid (clipped to the content region, drawn with scroll offset).
    int save_cx = 0, save_cy = 0, save_cw = 0, save_ch = 0;
    rf::gr::get_clip(&save_cx, &save_cy, &save_cw, &save_ch);
    rf::gr::set_clip(lo.content_x, lo.content_y, lo.content_w, lo.content_h);

    // Which cell is the cursor over (only if it is inside the visible content region)?
    int hovered = -1;

    for (int i = 0; i < lo.count; ++i) {
        int cx = 0, cy = 0;
        cell_rect(lo, i, cx, cy); // ABSOLUTE screen coords (used for hit-test + cull)

        // Skip cells fully outside the content region (cheap vertical cull, absolute coords).
        if (cy + lo.thumb < lo.content_y || cy > lo.content_y + lo.content_h) {
            continue;
        }

        if (cursor_in_content && point_in(mx, my, cx, cy, lo.thumb, lo.thumb)) {
            hovered = i;
        }

        // set_clip moved the drawing origin to (content_x, content_y), so everything drawn while
        // the clip is active must use coords RELATIVE to that origin. The absolute cx/cy above are
        // still what hit-testing (and the cursor) use, so draw at cx-content_x / cy-content_y.
        const int dx = cx - lo.content_x;
        const int dy = cy - lo.content_y;

        const int bm = spray_get_bitmap(static_cast<uint16_t>(i));
        if (bm >= 0) {
            int bw = 0, bh = 0;
            rf::bm::get_dimensions(bm, &bw, &bh);
            rf::gr::set_color(255, 255, 255, 255);
            rf::gr::bitmap_scaled(bm, dx, dy, lo.thumb, lo.thumb, 0, 0, bw, bh, false, false,
                rf::gr::bitmap_clamp_mode);
        }
        else {
            // Placeholder tile + (truncated) filename for a texture that failed to load.
            rf::gr::set_color(50, 50, 50, 255);
            rf::gr::rect(dx, dy, lo.thumb, lo.thumb);
            rf::gr::set_color(90, 90, 90, 255);
            hud_rect_border(dx, dy, lo.thumb, lo.thumb, 1);
            const char* name = spray_texture_name(static_cast<uint16_t>(i));
            if (name) {
                const std::string fit = hud_fit_string(name, lo.thumb - static_cast<int>(8 * lo.scale),
                    nullptr, font);
                rf::gr::set_color(200, 200, 200, 255);
                rf::gr::string_aligned(rf::gr::ALIGN_CENTER, dx + lo.thumb / 2, dy + lo.thumb / 2,
                    fit.c_str(), font);
            }
        }

        // Selected: bright green border. Hovered: subtle white border.
        if (i == g_alpine_game_config.selected_spray_index) {
            rf::gr::set_color(120, 230, 120, 255);
            hud_rect_border(dx - 2, dy - 2, lo.thumb + 4, lo.thumb + 4,
                std::max(2, static_cast<int>(3 * lo.scale)));
        }
        else if (i == hovered) {
            rf::gr::set_color(255, 255, 255, 200);
            hud_rect_border(dx - 1, dy - 1, lo.thumb + 2, lo.thumb + 2,
                std::max(1, static_cast<int>(2 * lo.scale)));
        }

        // Index badge: a small darkened box in the bottom-right corner showing this spray's number.
        {
            const std::string num = std::to_string(i);
            const int fh = rf::gr::get_font_height(font);
            const auto num_size = rf::gr::get_string_size(num, font);
            const int bpad = std::max(2, static_cast<int>(3 * lo.scale));
            const int box_w = num_size.first + bpad * 2;
            const int box_h = fh + bpad;
            const int box_x = dx + lo.thumb - box_w;
            const int box_y = dy + lo.thumb - box_h;
            rf::gr::set_color(0, 0, 0, 180);
            rf::gr::rect(box_x, box_y, box_w, box_h);
            rf::gr::set_color(255, 255, 255, 255);
            rf::gr::string_aligned(rf::gr::ALIGN_CENTER, box_x + box_w / 2,
                box_y + (box_h - fh) / 2, num.c_str(), font);
        }
    }

    // Restore the caller's clip window before drawing chrome outside the content region.
    rf::gr::set_clip(save_cx, save_cy, save_cw, save_ch);

    // Scrollbar if the grid overflows the content region.
    const float ms = max_scroll(lo);
    if (ms > 0.0f) {
        const float ratio = g_scroll / ms;
        const float bar_h = static_cast<float>(lo.content_h) * lo.content_h / lo.total_grid_h;
        const float bar_y = lo.content_y + ratio * (lo.content_h - bar_h);
        const int bar_w = std::max(4, static_cast<int>(6 * lo.scale));
        const int bar_x = lo.content_x + lo.content_w - bar_w;
        rf::gr::set_color(140, 200, 160, 255);
        rf::gr::rect(bar_x, std::lround(bar_y), bar_w, std::lround(bar_h));
    }

    // Cancel button, brighten on hover.
    const bool cancel_hover = point_in(mx, my, lo.cancel_x, lo.cancel_y, lo.cancel_w, lo.cancel_h);
    rf::gr::set_color(cancel_hover ? 70 : 45, cancel_hover ? 70 : 45, cancel_hover ? 70 : 45, 255);
    rf::gr::rect(lo.cancel_x, lo.cancel_y, lo.cancel_w, lo.cancel_h);
    rf::gr::set_color(150, 150, 150, 255);
    hud_rect_border(lo.cancel_x, lo.cancel_y, lo.cancel_w, lo.cancel_h, 1);
    rf::gr::set_color(255, 255, 255, 255);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, lo.cancel_x + lo.cancel_w / 2,
        lo.cancel_y + (lo.cancel_h - rf::gr::get_font_height(font)) / 2, "Cancel (Esc)", font);
}

void spray_picker_handle_mouse(int x, int y)
{
    if (!g_open) {
        return;
    }

    g_cursor_x = x;
    g_cursor_y = y;

    const Layout lo = compute_layout();

    // Mouse wheel scrolls the grid (one row per notch).
    if (rf::mouse_dz != 0) {
        const float step = static_cast<float>(lo.thumb + lo.gap);
        g_scroll += (rf::mouse_dz > 0 ? -step : step);
        g_scroll = std::clamp(g_scroll, 0.0f, max_scroll(lo));
    }

    if (rf::mouse_was_button_pressed(0)) {
        // Cancel button closes without changing the selection.
        if (point_in(x, y, lo.cancel_x, lo.cancel_y, lo.cancel_w, lo.cancel_h)) {
            g_open = false;
            return;
        }
        // Clicks only count inside the visible content region (so scrolled-off cells never hit).
        if (point_in(x, y, lo.content_x, lo.content_y, lo.content_w, lo.content_h)) {
            for (int i = 0; i < lo.count; ++i) {
                int cx = 0, cy = 0;
                cell_rect(lo, i, cx, cy);
                if (point_in(x, y, cx, cy, lo.thumb, lo.thumb)) {
                    g_alpine_game_config.set_selected_spray_index(i);
                    g_open = false;
                    return;
                }
            }
        }
    }
}

void spray_picker_handle_key(rf::Key* key)
{
    if (!g_open) {
        return;
    }
    if (*key == rf::Key::KEY_ESC) {
        g_open = false;
    }
    // Swallow every key while the modal is open so nothing leaks to the options menu.
    *key = rf::Key::KEY_NONE;
}
