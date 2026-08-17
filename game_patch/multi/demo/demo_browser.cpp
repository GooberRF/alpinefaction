#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include "demo_browser.h"
#include "demo.h"
#include "demo_details.h"
#include "demo_file.h"
#include "../gametype.h"
#include "../multi.h"
#include "../../hud/hud_internal.h"
#include "../../rf/gameseq.h"
#include "../../rf/gr/gr.h"
#include "../../rf/gr/gr_font.h"
#include "../../rf/input.h"
#include "../../rf/ui.h"

namespace
{
    struct Entry
    {
        // Enumerator order determines row order in the list (up row, then folders, then demos)
        enum class Kind
        {
            up,
            folder,
            demo,
        };
        Kind kind = Kind::demo;
        std::string name; // row label: ".." for up, folder leaf name, demo leaf name (no extension)
        std::string level;
        std::string date_str;
        uint64_t start_time = 0;
        bool valid = false;
    };

    std::vector<Entry> g_entries;
    bool g_open = false;
    float g_scroll = 0.0f;
    int g_cursor_x = 0;
    int g_cursor_y = 0;
    // Current folder relative to <rf_root>\demos ("" = root); preserved across close/reopen
    // so the browser returns to the folder a demo was launched from.
    std::string g_current_dir;

    // Clicking a row opens a detail screen (info + final scoreboard) instead of playing
    // right away; Watch on that screen starts playback, Back/ESC returns to the list.
    enum class BrowserMode
    {
        list,
        detail,
    };
    BrowserMode g_mode = BrowserMode::list;
    std::string g_detail_name;
    DemoDetails g_detail;
    float g_detail_scroll = 0.0f;

    // Scoreboard display lines (team section headers interleaved with player rows),
    // precomputed when the detail screen opens so scrolling/rendering stays trivial.
    struct DetailLine
    {
        enum class Kind
        {
            player,
            red_header,
            blue_header,
            other_header,
        };
        Kind kind = Kind::player;
        int row = -1; // index into g_detail.rows when kind == player
    };
    std::vector<DetailLine> g_detail_lines;

    constexpr float REF_WIDTH = 1280.0f;
    constexpr float REF_HEIGHT = 800.0f;

    struct Layout
    {
        float scale;
        int px, py, pw, ph;                             // panel rect
        int content_x, content_y, content_w, content_h; // scrollable list region (clip window)
        int gap;                                        // spacing between rows / edges
        int row_h;
        int count;
        int total_list_h;                               // full (unclipped) height of all rows
        int back_x, back_y, back_w, back_h;             // Back button rect
    };

    Layout compute_layout()
    {
        Layout lo{};
        const int clip_w = rf::gr::clip_width();
        const int clip_h = rf::gr::clip_height();
        lo.scale = std::min(clip_w / REF_WIDTH, clip_h / REF_HEIGHT);

        const auto s = [&](float v) { return static_cast<int>(v * lo.scale); };

        // Compact panel that leaves a clear margin at every resolution.
        lo.pw = std::min(s(860.0f), clip_w - 80);
        lo.ph = std::min(s(600.0f), clip_h - 80);
        lo.px = (clip_w - lo.pw) / 2;
        lo.py = (clip_h - lo.ph) / 2;

        const int pad = s(16.0f);
        const int title_h = s(46.0f);
        const int footer_h = s(58.0f);

        lo.gap = std::max(2, s(6.0f));
        lo.row_h = rf::gr::get_font_height(rf::ui::medium_font_0) + std::max(6, s(14.0f));

        lo.content_x = lo.px + pad;
        lo.content_y = lo.py + title_h;
        lo.content_w = lo.pw - 2 * pad;
        lo.content_h = lo.ph - title_h - footer_h;

        lo.count = static_cast<int>(g_entries.size());
        lo.total_list_h = lo.count * (lo.row_h + lo.gap) + lo.gap;

        lo.back_w = std::min(s(240.0f), lo.content_w);
        lo.back_h = s(40.0f);
        lo.back_x = lo.px + (lo.pw - lo.back_w) / 2;
        lo.back_y = lo.py + lo.ph - footer_h + (footer_h - lo.back_h) / 2;

        return lo;
    }

    // Screen rect of row `i`, accounting for the current scroll offset.
    void row_rect(const Layout& lo, int i, int& out_x, int& out_y, int& out_w)
    {
        out_x = lo.content_x + lo.gap;
        out_y = lo.content_y + lo.gap + i * (lo.row_h + lo.gap) - static_cast<int>(g_scroll);
        out_w = lo.content_w - 2 * lo.gap;
    }

    float max_scroll(const Layout& lo)
    {
        return std::max(0.0f, static_cast<float>(lo.total_list_h - lo.content_h));
    }

    bool point_in(int px, int py, int x, int y, int w, int h)
    {
        return px >= x && px < x + w && py >= y && py < y + h;
    }

    std::string join_rel(const std::string& dir, const std::string& leaf)
    {
        return dir.empty() ? leaf : dir + "\\" + leaf;
    }

    int dir_depth(const std::string& rel_dir)
    {
        if (rel_dir.empty())
            return 0;
        return 1 + static_cast<int>(std::count(rel_dir.begin(), rel_dir.end(), '\\'));
    }

    // (Re)populates g_entries from the current folder and resets the scroll position.
    void reload_entries()
    {
        g_scroll = 0.0f;
        g_entries.clear();

        DemoDirListing listing = demo_file_list_dir(g_current_dir);
        if (!listing.ok && !g_current_dir.empty()) {
            // Folder disappeared on disk (or exceeds the path budget) - fall back to the root
            g_current_dir.clear();
            listing = demo_file_list_dir(g_current_dir);
        }

        if (!g_current_dir.empty()) {
            Entry up;
            up.kind = Entry::Kind::up;
            up.name = "..";
            up.valid = true;
            g_entries.push_back(std::move(up));
        }
        // At the max depth deeper folders are simply not shown
        if (dir_depth(g_current_dir) < demo_max_dir_depth) {
            for (const auto& dir_name : listing.dirs) {
                Entry entry;
                entry.kind = Entry::Kind::folder;
                entry.name = dir_name;
                entry.valid = true;
                g_entries.push_back(std::move(entry));
            }
        }
        for (const auto& name : listing.names) {
            Entry entry;
            entry.name = name;
            DemoFileReader reader;
            const auto open_result = reader.open(demo_file_resolve_path(join_rel(g_current_dir, name)));
            if (open_result == DemoFileReader::OpenResult::ok
                || open_result == DemoFileReader::OpenResult::missing_features) {
                entry.valid = true;
                entry.level = reader.header().level_filename;
                entry.start_time = reader.header().start_time_unix;
                if (entry.start_time != 0) {
                    std::time_t t = static_cast<std::time_t>(entry.start_time);
                    char date_buf[32];
                    std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %H:%M", std::localtime(&t));
                    entry.date_str = date_buf;
                }
            }
            g_entries.push_back(std::move(entry));
        }
        // Up row first, then folders A-Z, then demos newest-first; stable keeps the name-sorted
        // order for folders and for demos without a timestamp
        std::stable_sort(g_entries.begin(), g_entries.end(), [](const Entry& a, const Entry& b) {
            if (a.kind != b.kind)
                return static_cast<int>(a.kind) < static_cast<int>(b.kind);
            return a.kind == Entry::Kind::demo && a.start_time > b.start_time;
        });
    }

    // Strips the last component off g_current_dir (shared by the ".." row, Back button and ESC).
    void navigate_up()
    {
        auto sep = g_current_dir.find_last_of('\\');
        g_current_dir = sep == std::string::npos ? std::string{} : g_current_dir.substr(0, sep);
        reload_entries();
    }

    struct DetailLayout
    {
        float scale;
        int px, py, pw, ph;                             // panel rect
        int info_x, info_y, info_w;                     // info block (label/value lines)
        int info_line_h;
        int header_y;                                   // fixed scoreboard header row
        int content_x, content_y, content_w, content_h; // scrollable score lines (clip window)
        int gap;
        int row_h;
        int count;
        int total_list_h;
        int watch_x, watch_y, watch_w, watch_h; // zero-sized when the demo is unreadable
        int back_x, back_y, back_w, back_h;
        bool show_caps;
        int col_score_r, col_caps_r, col_ping_r; // column right edges relative to content_x
    };

    DetailLayout compute_detail_layout()
    {
        DetailLayout lo{};
        const int clip_w = rf::gr::clip_width();
        const int clip_h = rf::gr::clip_height();
        lo.scale = std::min(clip_w / REF_WIDTH, clip_h / REF_HEIGHT);

        const auto s = [&](float v) { return static_cast<int>(v * lo.scale); };

        // Same panel as the list so switching modes doesn't visually jump.
        lo.pw = std::min(s(860.0f), clip_w - 80);
        lo.ph = std::min(s(600.0f), clip_h - 80);
        lo.px = (clip_w - lo.pw) / 2;
        lo.py = (clip_h - lo.ph) / 2;

        const int pad = s(16.0f);
        const int title_h = s(46.0f);
        const int footer_h = s(58.0f);
        const int fh = rf::gr::get_font_height(rf::ui::medium_font_0);

        lo.gap = std::max(2, s(6.0f));
        lo.row_h = fh + std::max(4, s(10.0f));

        lo.info_x = lo.px + pad;
        lo.info_y = lo.py + title_h;
        lo.info_w = lo.pw - 2 * pad;
        lo.info_line_h = fh + std::max(2, s(6.0f));
        const int info_lines = g_detail.readable ? (g_detail.header.mod_name.empty() ? 3 : 4) : 0;

        lo.header_y = lo.info_y + info_lines * lo.info_line_h + lo.gap;

        lo.content_x = lo.px + pad;
        lo.content_y = lo.header_y + (g_detail.readable ? lo.row_h : 0);
        lo.content_w = lo.pw - 2 * pad;
        lo.content_h = lo.py + lo.ph - footer_h - lo.content_y;

        lo.count = static_cast<int>(g_detail_lines.size());
        lo.total_list_h = lo.count * (lo.row_h + lo.gap) + lo.gap;

        const int btn_w = std::min(s(240.0f), (lo.content_w - 2 * lo.gap) / 2);
        const int btn_h = s(40.0f);
        const int btn_y = lo.py + lo.ph - footer_h + (footer_h - btn_h) / 2;
        lo.back_w = btn_w;
        lo.back_h = btn_h;
        lo.back_y = btn_y;
        if (g_detail.readable) {
            lo.watch_w = btn_w;
            lo.watch_h = btn_h;
            lo.watch_x = lo.px + lo.pw / 2 - btn_w - lo.gap;
            lo.watch_y = btn_y;
            lo.back_x = lo.px + lo.pw / 2 + lo.gap;
        }
        else {
            lo.back_x = lo.px + (lo.pw - btn_w) / 2;
        }

        const int text_pad = std::max(4, s(8.0f));
        lo.show_caps = g_detail.header.game_type == rf::NG_TYPE_CTF || g_detail.header.game_type == rf::NG_TYPE_SAL;
        lo.col_ping_r = lo.content_w - std::max(6, s(12.0f)) - text_pad; // stays clear of the scrollbar
        lo.col_caps_r = lo.col_ping_r - s(70.0f);
        lo.col_score_r = (lo.show_caps ? lo.col_caps_r : lo.col_ping_r) - s(80.0f);

        return lo;
    }

    float detail_max_scroll(const DetailLayout& lo)
    {
        return std::max(0.0f, static_cast<float>(lo.total_list_h - lo.content_h));
    }

    std::string format_duration(uint32_t duration_ms)
    {
        const uint32_t total_s = duration_ms / 1000;
        char buf[32];
        if (total_s >= 3600) {
            std::snprintf(buf, sizeof(buf), "%u:%02u:%02u", total_s / 3600, (total_s / 60) % 60, total_s % 60);
        }
        else {
            std::snprintf(buf, sizeof(buf), "%u:%02u", total_s / 60, total_s % 60);
        }
        return buf;
    }

    void open_detail(const Entry& entry)
    {
        // Relative path including subfolders - resolvable by demo_file_resolve_path and doubles
        // as the detail screen's breadcrumb title
        const std::string rel_name = join_rel(g_current_dir, entry.name);
        g_detail = demo_scan_details(rel_name);
        g_detail_name = rel_name;
        g_detail_scroll = 0.0f;
        g_detail_lines.clear();

        const bool team_mode = g_detail.readable && !g_detail.rows.empty()
            && multi_game_type_is_team_type(static_cast<rf::NetGameType>(g_detail.header.game_type));
        if (team_mode) {
            g_detail_lines.push_back({DetailLine::Kind::red_header, -1});
            for (int i = 0; i < static_cast<int>(g_detail.rows.size()); ++i) {
                if (g_detail.rows[i].team == 0) {
                    g_detail_lines.push_back({DetailLine::Kind::player, i});
                }
            }
            g_detail_lines.push_back({DetailLine::Kind::blue_header, -1});
            for (int i = 0; i < static_cast<int>(g_detail.rows.size()); ++i) {
                if (g_detail.rows[i].team == 1) {
                    g_detail_lines.push_back({DetailLine::Kind::player, i});
                }
            }
            bool other_header_added = false;
            for (int i = 0; i < static_cast<int>(g_detail.rows.size()); ++i) {
                if (g_detail.rows[i].team < 0) {
                    if (!other_header_added) {
                        g_detail_lines.push_back({DetailLine::Kind::other_header, -1});
                        other_header_added = true;
                    }
                    g_detail_lines.push_back({DetailLine::Kind::player, i});
                }
            }
        }
        else {
            for (int i = 0; i < static_cast<int>(g_detail.rows.size()); ++i) {
                g_detail_lines.push_back({DetailLine::Kind::player, i});
            }
        }

        g_mode = BrowserMode::detail;
    }

    void demo_browser_close()
    {
        g_open = false;
        g_scroll = 0.0f;
        g_mode = BrowserMode::list;
        g_detail = {};
        g_detail_lines.clear();
        g_detail_scroll = 0.0f;
    }

    void handle_detail_mouse(int x, int y)
    {
        const DetailLayout lo = compute_detail_layout();

        // Mouse wheel scrolls the scoreboard (one line per notch).
        if (rf::mouse_dz != 0) {
            const float step = static_cast<float>(lo.row_h + lo.gap);
            g_detail_scroll += (rf::mouse_dz > 0 ? -step : step);
            g_detail_scroll = std::clamp(g_detail_scroll, 0.0f, detail_max_scroll(lo));
        }

        if (rf::mouse_was_button_pressed(0)) {
            if (lo.watch_w > 0 && !g_detail.requires_newer
                && point_in(x, y, lo.watch_x, lo.watch_y, lo.watch_w, lo.watch_h)) {
                // On failure the wrapper prints the reason; keep the detail screen open
                if (demo_playback_start_from_menu(g_detail_name)) {
                    demo_browser_close();
                }
                return;
            }
            if (point_in(x, y, lo.back_x, lo.back_y, lo.back_w, lo.back_h)) {
                g_mode = BrowserMode::list; // list scroll position is intentionally preserved
            }
        }
    }

    void demo_browser_handle_mouse(int x, int y)
    {
        if (!g_open) {
            return;
        }

        g_cursor_x = x;
        g_cursor_y = y;

        if (g_mode == BrowserMode::detail) {
            handle_detail_mouse(x, y);
            return;
        }

        const Layout lo = compute_layout();

        // Mouse wheel scrolls the list (one row per notch).
        if (rf::mouse_dz != 0) {
            const float step = static_cast<float>(lo.row_h + lo.gap);
            g_scroll += (rf::mouse_dz > 0 ? -step : step);
            g_scroll = std::clamp(g_scroll, 0.0f, max_scroll(lo));
        }

        if (rf::mouse_was_button_pressed(0)) {
            // Back button mirrors ESC: up one folder, or close at the root.
            if (point_in(x, y, lo.back_x, lo.back_y, lo.back_w, lo.back_h)) {
                if (!g_current_dir.empty()) {
                    navigate_up();
                }
                else {
                    demo_browser_close();
                }
                return;
            }
            // Clicks only count inside the visible content region (so scrolled-off rows never hit).
            if (point_in(x, y, lo.content_x, lo.content_y, lo.content_w, lo.content_h)) {
                for (int i = 0; i < lo.count; ++i) {
                    int rx = 0, ry = 0, rw = 0;
                    row_rect(lo, i, rx, ry, rw);
                    if (point_in(x, y, rx, ry, rw, lo.row_h)) {
                        const Entry& entry = g_entries[i];
                        switch (entry.kind) {
                        case Entry::Kind::up:
                            navigate_up();
                            break;
                        case Entry::Kind::folder:
                            // Update g_current_dir before reloading - reload_entries invalidates `entry`
                            g_current_dir = join_rel(g_current_dir, entry.name);
                            reload_entries();
                            break;
                        case Entry::Kind::demo:
                            if (entry.valid) {
                                open_detail(entry);
                            }
                            break;
                        }
                        return;
                    }
                }
            }
        }
    }

    void demo_browser_handle_key(int key)
    {
        if (!g_open) {
            return;
        }
        if (key == rf::Key::KEY_ESC) {
            if (g_mode == BrowserMode::detail) {
                g_mode = BrowserMode::list;
            }
            else if (!g_current_dir.empty()) {
                navigate_up();
            }
            else {
                demo_browser_close();
            }
        }
        // Every other key is swallowed by the FunHook below so nothing leaks to the extras menu
    }

    void draw_button(int x, int y, int w, int h, const char* label, int font, int fh)
    {
        const bool hover = point_in(g_cursor_x, g_cursor_y, x, y, w, h);
        rf::gr::set_color(hover ? 70 : 45, hover ? 70 : 45, hover ? 70 : 45, 255);
        rf::gr::rect(x, y, w, h);
        rf::gr::set_color(150, 150, 150, 255);
        hud_rect_border(x, y, w, h, 1);
        rf::gr::set_color(255, 255, 255, 255);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, x + w / 2, y + (h - fh) / 2, label, font);
    }

    // One "Label: value" cell of the two-column info block (col 0 = left, 1 = right).
    void draw_info_pair(const DetailLayout& lo, int font, int col, int line, const char* label,
                        const std::string& value)
    {
        const int col_w = lo.info_w / 2 - lo.gap;
        const int x = lo.info_x + col * (lo.info_w / 2);
        const int y = lo.info_y + line * lo.info_line_h;
        const int indent = static_cast<int>(110 * lo.scale);
        rf::gr::set_color(160, 160, 160, 255);
        rf::gr::string(x, y, label, font);
        rf::gr::set_color(255, 255, 255, 255);
        const std::string fitted = hud_fit_string(value, std::max(40, col_w - indent), nullptr, font);
        rf::gr::string(x + indent, y, fitted.c_str(), font);
    }

    void render_detail()
    {
        const DetailLayout lo = compute_detail_layout();
        g_detail_scroll = std::clamp(g_detail_scroll, 0.0f, detail_max_scroll(lo));

        const int font = rf::ui::medium_font_0;
        const int fh = rf::gr::get_font_height(font);
        const int text_pad = std::max(4, static_cast<int>(8 * lo.scale));

        // Full-screen dim + panel chrome (matches the list screen).
        rf::gr::set_color(0, 0, 0, 192);
        rf::gr::rect(0, 0, rf::gr::clip_width(), rf::gr::clip_height());
        rf::gr::set_color(20, 20, 20, 235);
        rf::gr::rect(lo.px, lo.py, lo.pw, lo.ph);
        rf::gr::set_color(120, 120, 120, 255);
        hud_rect_border(lo.px, lo.py, lo.pw, lo.ph, std::max(1, static_cast<int>(2 * lo.scale)));

        rf::gr::set_color(255, 255, 255, 255);
        const std::string title = hud_fit_string(g_detail_name, lo.pw - 2 * text_pad, nullptr, font);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, lo.px + lo.pw / 2,
            lo.py + static_cast<int>(14 * lo.scale), title.c_str(), font);

        if (!g_detail.readable) {
            rf::gr::set_color(200, 200, 200, 255);
            rf::gr::string_aligned(rf::gr::ALIGN_CENTER, lo.content_x + lo.content_w / 2,
                lo.content_y + lo.content_h / 2 - fh / 2, "Could not read demo file", font);
            draw_button(lo.back_x, lo.back_y, lo.back_w, lo.back_h, "Back (Esc)", font, fh);
            return;
        }

        const DemoHeaderInfo& hdr = g_detail.header;

        std::string date_str = "-";
        if (hdr.start_time_unix != 0) {
            std::time_t t = static_cast<std::time_t>(hdr.start_time_unix);
            char date_buf[32];
            std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %H:%M", std::localtime(&t));
            date_str = date_buf;
        }
        std::string duration = format_duration(g_detail.duration_ms);
        if (!g_detail.has_footer) {
            duration += " (incomplete)";
        }
        std::string players_str = std::to_string(g_detail.rows.size());
        if (hdr.server_max_players > 0) {
            players_str += " / " + std::to_string(hdr.server_max_players);
        }

        draw_info_pair(lo, font, 0, 0, "Level:", hdr.level_filename);
        draw_info_pair(lo, font, 1, 0, "Date:", date_str);
        draw_info_pair(lo, font, 0, 1, "Type:",
            std::string{multi_game_type_name(static_cast<rf::NetGameType>(hdr.game_type))});
        draw_info_pair(lo, font, 1, 1, "Duration:", duration);
        draw_info_pair(lo, font, 0, 2, "Server:", hdr.server_name.empty() ? std::string{"-"} : hdr.server_name);
        draw_info_pair(lo, font, 1, 2, "Players:", players_str);
        if (!hdr.mod_name.empty()) {
            draw_info_pair(lo, font, 0, 3, "Mod:", hdr.mod_name);
        }

        // Scoreboard column header (fixed, above the scrolled region).
        const int hy = lo.header_y + (lo.row_h - fh) / 2;
        rf::gr::set_color(160, 160, 160, 255);
        rf::gr::string(lo.content_x + text_pad, hy, "Player", font);
        rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, lo.content_x + lo.col_score_r, hy, "Score", font);
        if (lo.show_caps) {
            rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, lo.content_x + lo.col_caps_r, hy, "Caps", font);
        }
        rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, lo.content_x + lo.col_ping_r, hy, "Ping", font);
        rf::gr::set_color(120, 120, 120, 255);
        rf::gr::rect(lo.content_x, lo.content_y - 1, lo.content_w, 1);

        if (g_detail.rows.empty()) {
            rf::gr::set_color(200, 200, 200, 255);
            rf::gr::string_aligned(rf::gr::ALIGN_CENTER, lo.content_x + lo.content_w / 2,
                lo.content_y + lo.content_h / 2 - fh / 2, "No players recorded", font);
        }

        // Scoreboard lines (clipped to the content region, drawn with scroll offset).
        int save_cx = 0, save_cy = 0, save_cw = 0, save_ch = 0;
        rf::gr::get_clip(&save_cx, &save_cy, &save_cw, &save_ch);
        rf::gr::set_clip(lo.content_x, lo.content_y, lo.content_w, lo.content_h);

        for (int i = 0; i < lo.count; ++i) {
            const int ry = lo.content_y + lo.gap + i * (lo.row_h + lo.gap) - static_cast<int>(g_detail_scroll);
            if (ry + lo.row_h < lo.content_y || ry > lo.content_y + lo.content_h) {
                continue;
            }
            // Coords inside the clip are relative to its origin (see the list renderer);
            // the column right edges are already content-relative.
            const int dy = ry - lo.content_y;
            const int text_y = dy + (lo.row_h - fh) / 2;
            const DetailLine& line = g_detail_lines[i];

            if (line.kind != DetailLine::Kind::player) {
                std::string label;
                if (line.kind == DetailLine::Kind::red_header) {
                    rf::gr::set_color(255, 90, 90, 255);
                    label = g_detail.team_scores_known ? "RED TEAM - " + std::to_string(g_detail.red_score)
                                                       : "RED TEAM";
                }
                else if (line.kind == DetailLine::Kind::blue_header) {
                    rf::gr::set_color(100, 140, 255, 255);
                    label = g_detail.team_scores_known ? "BLUE TEAM - " + std::to_string(g_detail.blue_score)
                                                       : "BLUE TEAM";
                }
                else {
                    rf::gr::set_color(170, 170, 170, 255);
                    label = "OTHER";
                }
                rf::gr::string(text_pad, text_y, label.c_str(), font);
                continue;
            }

            const DemoScoreRow& row = g_detail.rows[line.row];
            rf::gr::set_color(40, 40, 40, 180);
            rf::gr::rect(lo.gap, dy, lo.content_w - 2 * lo.gap, lo.row_h);

            rf::gr::set_color(255, 255, 255, 255);
            const int name_max_w = lo.col_score_r - static_cast<int>(90 * lo.scale) - 2 * text_pad;
            const std::string name = hud_fit_string(row.name, std::max(40, name_max_w), nullptr, font);
            rf::gr::string(text_pad, text_y, name.c_str(), font);

            rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, lo.col_score_r, text_y,
                std::to_string(row.score).c_str(), font);
            rf::gr::set_color(200, 200, 200, 255);
            if (lo.show_caps) {
                rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, lo.col_caps_r, text_y,
                    row.has_stats ? std::to_string(row.caps).c_str() : "-", font);
            }
            rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, lo.col_ping_r, text_y,
                row.has_stats ? std::to_string(row.ping).c_str() : "-", font);
        }

        rf::gr::set_clip(save_cx, save_cy, save_cw, save_ch);

        // Scrollbar if the scoreboard overflows the content region.
        const float ms = detail_max_scroll(lo);
        if (ms > 0.0f) {
            const float ratio = g_detail_scroll / ms;
            const float bar_h = static_cast<float>(lo.content_h) * lo.content_h / lo.total_list_h;
            const float bar_y = lo.content_y + ratio * (lo.content_h - bar_h);
            const int bar_w = std::max(4, static_cast<int>(6 * lo.scale));
            const int bar_x = lo.content_x + lo.content_w - bar_w;
            rf::gr::set_color(140, 200, 160, 255);
            rf::gr::rect(bar_x, std::lround(bar_y), bar_w, std::lround(bar_h));
        }

        if (g_detail.requires_newer) {
            rf::gr::set_color(200, 200, 200, 255);
            rf::gr::string_aligned(rf::gr::ALIGN_CENTER, lo.watch_x + lo.watch_w / 2,
                lo.watch_y + (lo.watch_h - fh) / 2, "Requires newer Alpine Faction", font);
        }
        else {
            draw_button(lo.watch_x, lo.watch_y, lo.watch_w, lo.watch_h, "Watch", font, fh);
        }
        draw_button(lo.back_x, lo.back_y, lo.back_w, lo.back_h, "Back (Esc)", font, fh);
    }

    rf::ui::Button g_extras_demos_btn;

    void extras_demos_btn_on_click(int, int)
    {
        demo_browser_open();
        // Stock click dispatch plays the button sound after this returns
    }

    // demo_extras_init registers its 4 buttons (credits, summoner, glass house, back) into two
    // FArray_Ptr_5 lists: 0x0063B500 (render + hit-test) and 0x0063B4A8 (focus/click/key) - the
    // stock click dispatch hit-tests list1[i] then calls list2[i]->on_click, so both lists must
    // stay parallel. Capacity is 5, leaving exactly one free slot for the Demos button. This
    // injection point is in the init tail that runs on every menu entry (the one-shot creation
    // block above it is guarded by 0x0063B560), hence the own static guard. It sits at the
    // `call demo_extras_open` instruction rather than the tail's start at 0x0043F02D because
    // the fild there is not decodable by subhook (null trampoline = jump to address 0).
    CodeInjection extras_init_demos_button_injection{
        0x0043F043,
        []() {
            static bool created = false;
            if (created) {
                return;
            }
            created = true;

            auto& back_btn = addr_as_ref<rf::ui::Button>(0x0063B390);
            g_extras_demos_btn.init();
            g_extras_demos_btn.create("button.tga", "button_selected.tga",
                addr_as_ref<int>(0x0063C050) /* g_MenuMainMinOffsetX */, back_btn.y,
                0x20 /* DIK_D */, "DEMOS", rf::ui::medium_font_0);
            g_extras_demos_btn.on_click = extras_demos_btn_on_click;
            g_extras_demos_btn.enabled = true;
            back_btn.y += g_extras_demos_btn.h;

            struct FArrayPtr5
            {
                int num;
                void* elements[5];
            };
            for (auto* list : {&addr_as_ref<FArrayPtr5>(0x0063B500), &addr_as_ref<FArrayPtr5>(0x0063B4A8)}) {
                list->elements[4] = list->elements[3]; // Back stays visually and navigationally last
                list->elements[3] = &g_extras_demos_btn;
                list->num = 5;
            }
        },
    };

    // demo_extras_render assigns the animated slide-in X (still in EAX here) to each stock
    // button's .x right before this point - give the Demos button the same treatment so it
    // slides in with the rest. The render loop over list1 draws it afterwards.
    CodeInjection extras_render_demos_btn_x_injection{
        0x0043EE23,
        [](auto& regs) {
            g_extras_demos_btn.x = regs.eax;
        },
    };

    FunHook<void()> extras_handle_mouse_move_hook{
        0x0043F230,
        []() {
            if (g_open) {
                int x = 0, y = 0, z = 0;
                rf::mouse_get_pos(x, y, z);
                demo_browser_handle_mouse(x, y);
                return; // do not run stock extras mouse handling while the browser is open
            }
            extras_handle_mouse_move_hook.call_target();
        },
    };

    FunHook<void(int)> extras_handle_key_hook{
        0x0043F120,
        [](int key) {
            if (g_open) {
                demo_browser_handle_key(key); // swallows everything (ESC must not trigger stock Back)
                return;
            }
            extras_handle_key_hook.call_target(key);
        },
    };
}

bool demo_browser_is_open()
{
    return g_open;
}

void demo_browser_open()
{
    g_open = true;
    g_mode = BrowserMode::list;
    // g_current_dir is intentionally kept so reopening after playback returns to the same
    // folder; reload_entries falls back to the root if it no longer exists.
    reload_entries();
}

void demo_browser_render()
{
    if (!g_open) {
        return;
    }

    // Close the browser if the game state changes (playback started, menu left) so it
    // doesn't get stuck open.
    if (rf::gameseq_get_state() != rf::GS_EXTRAS_MENU) {
        demo_browser_close();
        return;
    }

    if (g_mode == BrowserMode::detail) {
        render_detail();
        return;
    }

    const Layout lo = compute_layout();
    g_scroll = std::clamp(g_scroll, 0.0f, max_scroll(lo));

    const int font = rf::ui::medium_font_0;
    const int fh = rf::gr::get_font_height(font);

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

    const int text_pad = std::max(4, static_cast<int>(8 * lo.scale));

    // Title doubles as the breadcrumb for the current folder.
    const std::string title_str = g_current_dir.empty() ? std::string{"Demos"} : "Demos\\" + g_current_dir;
    const std::string title = hud_fit_string(title_str, lo.pw - 2 * text_pad, nullptr, font);
    rf::gr::set_color(255, 255, 255, 255);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, lo.px + lo.pw / 2,
        lo.py + static_cast<int>(14 * lo.scale), title.c_str(), font);

    // The ".." row alone doesn't count as content (it still renders above the message).
    const bool only_up = g_entries.size() == 1 && g_entries[0].kind == Entry::Kind::up;
    if (g_entries.empty() || only_up) {
        rf::gr::set_color(200, 200, 200, 255);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, lo.content_x + lo.content_w / 2,
            lo.content_y + lo.content_h / 2 - fh / 2,
            g_current_dir.empty() ? "No demos found" : "Folder is empty", font);
    }

    // Rows (clipped to the content region, drawn with scroll offset).
    int save_cx = 0, save_cy = 0, save_cw = 0, save_ch = 0;
    rf::gr::get_clip(&save_cx, &save_cy, &save_cw, &save_ch);
    rf::gr::set_clip(lo.content_x, lo.content_y, lo.content_w, lo.content_h);

    for (int i = 0; i < lo.count; ++i) {
        int rx = 0, ry = 0, rw = 0;
        row_rect(lo, i, rx, ry, rw); // ABSOLUTE screen coords (used for hit-test + cull)

        // Skip rows fully outside the content region (cheap vertical cull, absolute coords).
        if (ry + lo.row_h < lo.content_y || ry > lo.content_y + lo.content_h) {
            continue;
        }

        const Entry& entry = g_entries[i];
        const bool hovered = cursor_in_content && point_in(mx, my, rx, ry, rw, lo.row_h);

        // set_clip moved the drawing origin to (content_x, content_y), so everything drawn while
        // the clip is active must use coords RELATIVE to that origin. The absolute rx/ry above are
        // still what hit-testing (and the cursor) use, so draw at rx-content_x / ry-content_y.
        const int dx = rx - lo.content_x;
        const int dy = ry - lo.content_y;

        rf::gr::set_color(40, 40, 40, hovered && entry.valid ? 255 : 180);
        rf::gr::rect(dx, dy, rw, lo.row_h);
        if (hovered && entry.valid) {
            rf::gr::set_color(255, 255, 255, 200);
            hud_rect_border(dx, dy, rw, lo.row_h, std::max(1, static_cast<int>(2 * lo.scale)));
        }

        const int text_y = dy + (lo.row_h - fh) / 2;

        // Right-aligned metadata first, then fit the label into the remaining width.
        // A trailing backslash marks folder rows (the bitmap font is ASCII-only).
        std::string label = entry.name;
        std::string meta;
        switch (entry.kind) {
        case Entry::Kind::up:
            break;
        case Entry::Kind::folder:
            label += "\\";
            meta = "folder";
            break;
        case Entry::Kind::demo:
            meta = entry.valid
                ? (entry.date_str.empty() ? entry.level : entry.level + "   " + entry.date_str)
                : "unreadable";
            break;
        }
        int meta_w = 0;
        if (!meta.empty()) {
            meta = hud_fit_string(meta, rw / 2, &meta_w, font);
            rf::gr::set_color(entry.valid ? 170 : 120, entry.valid ? 170 : 120, entry.valid ? 170 : 120, 255);
            rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, dx + rw - text_pad, text_y, meta.c_str(), font);
        }

        const int name_max_w = std::max(rw / 4, rw - meta_w - 3 * text_pad);
        label = hud_fit_string(label, name_max_w, nullptr, font);
        if (entry.valid) {
            rf::gr::set_color(255, 255, 255, 255);
        }
        else {
            rf::gr::set_color(140, 140, 140, 255);
        }
        rf::gr::string(dx + text_pad, text_y, label.c_str(), font);
    }

    // Restore the caller's clip window before drawing chrome outside the content region.
    rf::gr::set_clip(save_cx, save_cy, save_cw, save_ch);

    // Scrollbar if the list overflows the content region.
    const float ms = max_scroll(lo);
    if (ms > 0.0f) {
        const float ratio = g_scroll / ms;
        const float bar_h = static_cast<float>(lo.content_h) * lo.content_h / lo.total_list_h;
        const float bar_y = lo.content_y + ratio * (lo.content_h - bar_h);
        const int bar_w = std::max(4, static_cast<int>(6 * lo.scale));
        const int bar_x = lo.content_x + lo.content_w - bar_w;
        rf::gr::set_color(140, 200, 160, 255);
        rf::gr::rect(bar_x, std::lround(bar_y), bar_w, std::lround(bar_h));
    }

    draw_button(lo.back_x, lo.back_y, lo.back_w, lo.back_h, "Back (Esc)", font, fh);
}

void demo_browser_apply_patch()
{
    extras_init_demos_button_injection.install();
    extras_render_demos_btn_x_injection.install();
    extras_handle_mouse_move_hook.install();
    extras_handle_key_hook.install();
}
