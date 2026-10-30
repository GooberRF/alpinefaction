#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>
#include <vector>
#include <common/utils/string-utils.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include "demo_browser.h"
#include "demo.h"
#include "demo_details.h"
#include "demo_file.h"
#include "../gametype.h"
#include "../map_thumbnails.h"
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
        std::string date_str;
        // Game type names, resolved once (an unknown type logs a warning per lookup)
        std::string type_short;
        std::string type_name;
        bool valid = false;
        // Demo needs features this build doesn't know - listed and previewed, but not playable
        bool requires_newer = false;
        DemoHeaderInfo header;
        // Final scoreboard etc. for the preview pane; needs a full scan, so it is filled on hover
        std::optional<DemoDetails> details;
    };

    std::vector<Entry> g_entries;
    bool g_open = false;
    float g_scroll = 0.0f;
    int g_cursor_x = 0;
    int g_cursor_y = 0;
    // Current folder relative to <rf_root>\demos ("" = root); preserved across close/reopen
    // so the browser returns to the folder a demo was launched from.
    std::string g_current_dir;

    // Demo row shown in the preview pane: the last one hovered, so the pane keeps its content
    // while the cursor leaves the list. -1 = none.
    int g_preview = -1;
    std::chrono::steady_clock::time_point g_preview_since;
    // The hover must rest this long before the demo is scanned, so sweeping the cursor down the
    // list doesn't scan every demo it crosses.
    constexpr auto preview_scan_delay = std::chrono::milliseconds{150};

    // Scoreboard display lines (team section headers interleaved with player rows).
    struct ScoreLine
    {
        enum class Kind
        {
            player,
            red_header,
            blue_header,
            other_header,
        };
        Kind kind = Kind::player;
        int row = -1; // index into DemoDetails::rows when kind == player
    };

    constexpr float REF_WIDTH = 1280.0f;
    constexpr float REF_HEIGHT = 800.0f;

    struct Layout
    {
        float scale;
        int px, py, pw, ph;                             // panel rect
        int text_pad;
        int col_gap;                                    // spacing between text columns
        int list_header_y;                              // fixed column header above the rows
        int content_x, content_y, content_w, content_h; // scrollable list region (clip window)
        int pane_x, pane_y, pane_w, pane_h;             // preview pane
        int gap;                                        // spacing between rows / edges
        int row_h;
        int bar_w;                                      // scrollbar width (rows stop short of it)
        int count;
        int total_list_h;                               // full (unclipped) height of all rows
        int back_x, back_y, back_w, back_h;             // Back button rect
        // Row-relative column positions
        int thumb_inset, thumb_w, thumb_h;
        int col_name_x;
        int col_level_x, col_level_w;
        int col_type_x, col_type_w;
        int col_date_x, col_date_w;
    };

    // List and preview text. The menu fonts grow with the screen height (see menu_init_hook), so
    // the medium font would leave the columns little room; it is kept for the title and button.
    int text_font()
    {
        return rf::ui::small_font;
    }

    int text_width(std::string_view text, int font)
    {
        return rf::gr::get_string_size(text, font).first;
    }

    // Width of the widest "YYYY-MM-DD HH:MM": the menu font's digits are proportional, so a
    // sample like all zeros can come out narrower than a real date.
    int date_column_width(int font)
    {
        static int cached_font = -1;
        static int cached_w = 0;
        if (font != cached_font) {
            char widest = '0';
            for (char digit = '1'; digit <= '9'; ++digit) {
                if (text_width({&digit, 1}, font) > text_width({&widest, 1}, font)) {
                    widest = digit;
                }
            }
            std::string sample = "0000-00-00 00:00";
            std::replace(sample.begin(), sample.end(), '0', widest);
            cached_font = font;
            cached_w = text_width(sample, font);
        }
        return cached_w;
    }

    Layout compute_layout()
    {
        Layout lo{};
        const int clip_w = rf::gr::clip_width();
        const int clip_h = rf::gr::clip_height();
        lo.scale = std::min(clip_w / REF_WIDTH, clip_h / REF_HEIGHT);

        const auto s = [&](float v) { return static_cast<int>(v * lo.scale); };
        const int font = text_font();
        const int fh = rf::gr::get_font_height(font);

        // Wide panel (list + preview pane) that leaves a clear margin at every resolution.
        lo.pw = std::min(s(1240.0f), clip_w - 40);
        lo.ph = std::min(s(740.0f), clip_h - 40);
        lo.px = (clip_w - lo.pw) / 2;
        lo.py = (clip_h - lo.ph) / 2;

        const int pad = s(16.0f);
        const int title_h = s(46.0f);
        const int footer_h = s(58.0f);

        lo.gap = std::max(2, s(6.0f));
        lo.text_pad = std::max(4, s(8.0f));
        lo.col_gap = std::max(8, s(16.0f));
        lo.row_h = std::max(fh + std::max(6, s(14.0f)), s(34.0f));
        lo.bar_w = std::max(4, s(6.0f));

        const int body_y = lo.py + title_h;
        const int body_h = lo.ph - title_h - footer_h;

        lo.pane_w = (lo.pw - 3 * pad) * 2 / 5;
        lo.pane_x = lo.px + lo.pw - pad - lo.pane_w;
        lo.pane_y = body_y;
        lo.pane_h = body_h;

        lo.list_header_y = body_y;
        lo.content_x = lo.px + pad;
        lo.content_y = body_y + fh + 2 * lo.gap;
        lo.content_w = lo.pane_x - pad - lo.content_x;
        lo.content_h = body_y + body_h - lo.content_y;

        lo.count = static_cast<int>(g_entries.size());
        lo.total_list_h = lo.count * (lo.row_h + lo.gap) + lo.gap;

        // Fixed-width columns are sized from the font so they never overlap; the name takes what
        // is left, sharing it with the level column.
        const int rw = lo.content_w - 2 * lo.gap - lo.bar_w;
        lo.thumb_inset = std::max(2, s(3.0f));
        lo.thumb_h = lo.row_h - 2 * lo.thumb_inset;
        lo.thumb_w = lo.thumb_h * 16 / 9;
        lo.col_name_x = lo.thumb_inset + lo.thumb_w + lo.text_pad;
        lo.col_date_w = date_column_width(font);
        lo.col_date_x = rw - lo.text_pad - lo.col_date_w;
        lo.col_type_w = std::max({text_width("Type", font), text_width("KOTH", font), text_width("SCAV", font),
            text_width("TBAG", font)});
        lo.col_type_x = lo.col_date_x - lo.col_gap - lo.col_type_w;
        const int name_and_level_w = lo.col_type_x - lo.col_gap - lo.col_name_x;
        lo.col_level_w = std::min(s(170.0f), (name_and_level_w - lo.col_gap) * 2 / 5);
        lo.col_level_x = lo.col_type_x - lo.col_gap - lo.col_level_w;

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
        out_w = lo.content_w - 2 * lo.gap - lo.bar_w;
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

    // Index of the row under the cursor, or -1 (rows scrolled out of the content region never hit).
    int row_at(const Layout& lo, int x, int y)
    {
        if (!point_in(x, y, lo.content_x, lo.content_y, lo.content_w, lo.content_h)) {
            return -1;
        }
        for (int i = 0; i < lo.count; ++i) {
            int rx = 0, ry = 0, rw = 0;
            row_rect(lo, i, rx, ry, rw);
            if (point_in(x, y, rx, ry, rw, lo.row_h)) {
                return i;
            }
        }
        return -1;
    }

    // (Re)populates g_entries from the current folder and resets the scroll position.
    void reload_entries()
    {
        g_scroll = 0.0f;
        g_preview = -1;
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
                entry.requires_newer = open_result == DemoFileReader::OpenResult::missing_features;
                entry.header = reader.header();
                const auto game_type = static_cast<rf::NetGameType>(entry.header.game_type);
                entry.type_short = multi_game_type_name_short(game_type);
                entry.type_name = multi_game_type_name(game_type);
                if (entry.header.start_time_unix != 0) {
                    std::time_t t = static_cast<std::time_t>(entry.header.start_time_unix);
                    // localtime returns NULL for out-of-range values; strftime(NULL) would
                    // trip the invalid-parameter handler and kill the process
                    if (const std::tm* tm = std::localtime(&t)) {
                        char date_buf[32];
                        std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %H:%M", tm);
                        entry.date_str = date_buf;
                    }
                }
            }
            g_entries.push_back(std::move(entry));
        }
        // Up row first, then folders A-Z, then demos newest-first; stable keeps the name-sorted
        // order for folders and for demos without a timestamp
        std::stable_sort(g_entries.begin(), g_entries.end(), [](const Entry& a, const Entry& b) {
            if (a.kind != b.kind)
                return static_cast<int>(a.kind) < static_cast<int>(b.kind);
            return a.kind == Entry::Kind::demo && a.header.start_time_unix > b.header.start_time_unix;
        });
    }

    // Strips the last component off g_current_dir (shared by the ".." row, Back button and ESC).
    void navigate_up()
    {
        auto sep = g_current_dir.find_last_of('\\');
        g_current_dir = sep == std::string::npos ? std::string{} : g_current_dir.substr(0, sep);
        reload_entries();
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

    // "ctf01.rfl" -> "ctf01"
    std::string level_display_name(const std::string& level_filename)
    {
        if (string_iends_with(level_filename, ".rfl")) {
            return level_filename.substr(0, level_filename.size() - 4);
        }
        return level_filename;
    }

    std::vector<ScoreLine> build_score_lines(const DemoDetails& details)
    {
        std::vector<ScoreLine> lines;
        const auto add_rows = [&](auto pred) {
            for (int i = 0; i < static_cast<int>(details.rows.size()); ++i) {
                if (pred(details.rows[i])) {
                    lines.push_back({ScoreLine::Kind::player, i});
                }
            }
        };
        const bool team_mode = !details.rows.empty()
            && multi_game_type_is_team_type(static_cast<rf::NetGameType>(details.header.game_type));
        if (!team_mode) {
            add_rows([](const DemoScoreRow&) { return true; });
            return lines;
        }
        lines.push_back({ScoreLine::Kind::red_header, -1});
        add_rows([](const DemoScoreRow& row) { return row.team == 0; });
        lines.push_back({ScoreLine::Kind::blue_header, -1});
        add_rows([](const DemoScoreRow& row) { return row.team == 1; });
        const bool has_other = std::any_of(details.rows.begin(), details.rows.end(),
            [](const DemoScoreRow& row) { return row.team < 0; });
        if (has_other) {
            lines.push_back({ScoreLine::Kind::other_header, -1});
            add_rows([](const DemoScoreRow& row) { return row.team < 0; });
        }
        return lines;
    }

    // Follows the hovered demo row and scans it once the hover has settled.
    void update_preview(int hovered)
    {
        const auto now = std::chrono::steady_clock::now();
        if (hovered >= 0 && hovered != g_preview) {
            g_preview = hovered;
            g_preview_since = now;
        }
        if (g_preview >= 0 && g_preview < static_cast<int>(g_entries.size())) {
            Entry& entry = g_entries[g_preview];
            if (!entry.details && now - g_preview_since >= preview_scan_delay) {
                entry.details = demo_scan_details(join_rel(g_current_dir, entry.name));
            }
        }
    }

    void demo_browser_close()
    {
        g_open = false;
        g_scroll = 0.0f;
        g_preview = -1;
        g_entries.clear();
        map_thumbnails_release_bitmaps();
    }

    void demo_browser_handle_mouse(int x, int y)
    {
        if (!g_open) {
            return;
        }

        g_cursor_x = x;
        g_cursor_y = y;

        const Layout lo = compute_layout();

        // Mouse wheel scrolls the list (one row per notch).
        if (rf::mouse_dz != 0) {
            const float step = static_cast<float>(lo.row_h + lo.gap);
            g_scroll += (rf::mouse_dz > 0 ? -step : step);
            g_scroll = std::clamp(g_scroll, 0.0f, max_scroll(lo));
        }

        if (!rf::mouse_was_button_pressed(0)) {
            return;
        }
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
        const int row = row_at(lo, x, y);
        if (row < 0) {
            return;
        }
        const Entry& entry = g_entries[row];
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
            // On failure the wrapper prints the reason; keep the browser open
            if (entry.valid && !entry.requires_newer
                && demo_playback_start_from_menu(join_rel(g_current_dir, entry.name))) {
                demo_browser_close();
            }
            break;
        }
    }

    void demo_browser_handle_key(int key)
    {
        if (!g_open) {
            return;
        }
        if (key == rf::Key::KEY_ESC) {
            if (!g_current_dir.empty()) {
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

    // The level's FactionFiles preview cropped to fill the box, or a dark placeholder until
    // (or unless) one is available. Returns whether the image was drawn.
    bool draw_level_thumbnail(const std::string& level_filename, int x, int y, int w, int h)
    {
        int img_w = 0, img_h = 0;
        const int bm = map_thumbnail_get(level_filename, &img_w, &img_h);
        if (bm == -1 || img_w <= 0 || img_h <= 0) {
            rf::gr::set_color(10, 10, 10, 255);
            rf::gr::rect(x, y, w, h);
            return false;
        }
        int sx = 0, sy = 0, sw = img_w, sh = img_h;
        if (img_w * h > w * img_h) {
            sw = img_h * w / h;
            sx = (img_w - sw) / 2;
        }
        else {
            sh = img_w * h / w;
            sy = (img_h - sh) / 2;
        }
        rf::gr::set_color(255, 255, 255, 255);
        rf::gr::bitmap_scaled(bm, x, y, w, h, sx, sy, sw, sh);
        return true;
    }

    // Right side of the panel: thumbnail, header info and final scoreboard of g_preview.
    void render_preview(const Layout& lo, int font, int fh)
    {
        rf::gr::set_color(30, 30, 30, 235);
        rf::gr::rect(lo.pane_x, lo.pane_y, lo.pane_w, lo.pane_h);
        rf::gr::set_color(80, 80, 80, 255);
        hud_rect_border(lo.pane_x, lo.pane_y, lo.pane_w, lo.pane_h, 1);

        const int x = lo.pane_x + lo.text_pad;
        const int w = lo.pane_w - 2 * lo.text_pad;
        const int bottom = lo.pane_y + lo.pane_h - lo.text_pad;

        if (g_preview < 0 || g_preview >= static_cast<int>(g_entries.size())) {
            rf::gr::set_color(150, 150, 150, 255);
            const std::string hint = hud_fit_string("Hover a demo to see its details", w, nullptr, font);
            rf::gr::string_aligned(rf::gr::ALIGN_CENTER, x + w / 2, lo.pane_y + lo.pane_h / 2 - fh / 2,
                hint.c_str(), font);
            return;
        }
        const Entry& entry = g_entries[g_preview];
        const DemoHeaderInfo& hdr = entry.header;
        int y = lo.pane_y + lo.text_pad;

        // Thumbnail with the level and game type captioned along its bottom edge
        const int thumb_h = w * 9 / 16;
        draw_level_thumbnail(hdr.level_filename, x, y, w, thumb_h);
        const int caption_h = fh + 2 * lo.gap;
        const int caption_y = y + thumb_h - caption_h;
        rf::gr::set_color(0, 0, 0, 170);
        rf::gr::rect(x, caption_y, w, caption_h);
        int type_w = 0;
        const std::string type_str = hud_fit_string(entry.type_name, w / 2, &type_w, font);
        rf::gr::set_color(200, 200, 200, 255);
        rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, x + w - lo.text_pad, caption_y + lo.gap, type_str.c_str(), font);
        const std::string level_str = hud_fit_string(level_display_name(hdr.level_filename),
            w - type_w - 3 * lo.text_pad, nullptr, font);
        rf::gr::set_color(255, 255, 255, 255);
        rf::gr::string(x + lo.text_pad, caption_y + lo.gap, level_str.c_str(), font);
        y += thumb_h + 2 * lo.gap;

        // Info lines: two label/value columns, the server name across the full width
        const DemoDetails* details = entry.details ? &*entry.details : nullptr;
        const std::string pending = "...";
        std::string duration = pending;
        std::string players = pending;
        if (details && details->readable) {
            duration = format_duration(details->duration_ms);
            if (!details->has_footer) {
                duration += " (incomplete)";
            }
            players = std::to_string(details->rows.size());
            if (hdr.server_max_players > 0) {
                players += " / " + std::to_string(hdr.server_max_players);
            }
        }
        const int line_h = fh + std::max(2, static_cast<int>(6 * lo.scale));
        const int label_w = text_width("Duration:", font) + lo.text_pad;
        const int half_w = w / 2;
        const auto info = [&](int col, int line, const char* label, const std::string& value, int cell_w) {
            const int cx = x + col * half_w;
            const int cy = y + line * line_h;
            rf::gr::set_color(160, 160, 160, 255);
            rf::gr::string(cx, cy, label, font);
            rf::gr::set_color(255, 255, 255, 255);
            const std::string fitted = hud_fit_string(value, std::max(20, cell_w - label_w - lo.text_pad), nullptr, font);
            rf::gr::string(cx + label_w, cy, fitted.c_str(), font);
        };
        info(0, 0, "Date:", entry.date_str.empty() ? std::string{"-"} : entry.date_str, half_w);
        info(1, 0, "Duration:", duration, half_w);
        info(0, 1, "Players:", players, half_w);
        if (!hdr.mod_name.empty()) {
            info(1, 1, "Mod:", hdr.mod_name, half_w);
        }
        info(0, 2, "Server:", hdr.server_name.empty() ? std::string{"-"} : hdr.server_name, w);
        int info_lines = 3;
        if (entry.requires_newer) {
            rf::gr::set_color(255, 170, 80, 255);
            rf::gr::string(x, y + info_lines * line_h, "Requires a newer Alpine Faction to play", font);
            ++info_lines;
        }
        y += info_lines * line_h + lo.gap;

        // Final scoreboard, cut off with a "+N more" line when it doesn't fit
        if (!details) {
            return;
        }
        if (!details->readable || details->rows.empty()) {
            rf::gr::set_color(170, 170, 170, 255);
            rf::gr::string(x, y, details->readable ? "No players recorded" : "Could not read demo file", font);
            return;
        }
        const bool show_caps = hdr.game_type == rf::NG_TYPE_CTF || hdr.game_type == rf::NG_TYPE_SAL
            || hdr.game_type == rf::NG_TYPE_SCAV;
        const int col_ping_r = x + w;
        const int col_caps_r = col_ping_r - text_width("Ping", font) - lo.col_gap;
        const int col_score_r = (show_caps ? col_caps_r - text_width("Caps", font) : col_ping_r - text_width("Ping", font))
            - lo.col_gap;
        const int name_max_w = col_score_r - text_width("Score", font) - lo.col_gap - x;

        rf::gr::set_color(160, 160, 160, 255);
        rf::gr::string(x, y, "Player", font);
        rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, col_score_r, y, "Score", font);
        if (show_caps) {
            rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, col_caps_r, y, "Caps", font);
        }
        rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, col_ping_r, y, "Ping", font);
        y += fh + lo.gap / 2;
        rf::gr::set_color(100, 100, 100, 255);
        rf::gr::rect(x, y, w, 1);
        y += lo.gap / 2 + 1;

        const std::vector<ScoreLine> lines = build_score_lines(*details);
        const int max_lines = std::max(0, (bottom - y) / line_h);
        const bool overflow = static_cast<int>(lines.size()) > max_lines;
        const int shown = overflow ? std::max(0, max_lines - 1) : static_cast<int>(lines.size());
        for (int i = 0; i < shown; ++i, y += line_h) {
            const ScoreLine& line = lines[i];
            if (line.kind != ScoreLine::Kind::player) {
                std::string label;
                if (line.kind == ScoreLine::Kind::red_header) {
                    rf::gr::set_color(255, 90, 90, 255);
                    label = details->team_scores_known ? "RED TEAM - " + std::to_string(details->red_score)
                                                       : "RED TEAM";
                }
                else if (line.kind == ScoreLine::Kind::blue_header) {
                    rf::gr::set_color(100, 140, 255, 255);
                    label = details->team_scores_known ? "BLUE TEAM - " + std::to_string(details->blue_score)
                                                       : "BLUE TEAM";
                }
                else {
                    rf::gr::set_color(170, 170, 170, 255);
                    label = "OTHER";
                }
                rf::gr::string(x, y, label.c_str(), font);
                continue;
            }
            const DemoScoreRow& row = details->rows[line.row];
            rf::gr::set_color(255, 255, 255, 255);
            const std::string name = hud_fit_string(row.name, std::max(20, name_max_w), nullptr, font);
            rf::gr::string(x, y, name.c_str(), font);
            rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, col_score_r, y, std::to_string(row.score).c_str(), font);
            rf::gr::set_color(200, 200, 200, 255);
            if (show_caps) {
                rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, col_caps_r, y,
                    row.has_stats ? std::to_string(row.caps).c_str() : "-", font);
            }
            rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, col_ping_r, y,
                row.has_stats ? std::to_string(row.ping).c_str() : "-", font);
        }
        if (overflow) {
            const int more_players = static_cast<int>(std::count_if(lines.begin() + shown, lines.end(),
                [](const ScoreLine& line) { return line.kind == ScoreLine::Kind::player; }));
            rf::gr::set_color(150, 150, 150, 255);
            rf::gr::string(x, y, ("+" + std::to_string(more_players) + " more").c_str(), font);
        }
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

    const Layout lo = compute_layout();
    g_scroll = std::clamp(g_scroll, 0.0f, max_scroll(lo));

    const int font = text_font();
    const int fh = rf::gr::get_font_height(font);
    const int title_font = rf::ui::medium_font_0;
    const int title_fh = rf::gr::get_font_height(title_font);

    // Use the stored menu cursor (screen/clip space) for hover feedback so it matches hit-testing.
    const int hovered = row_at(lo, g_cursor_x, g_cursor_y);

    // Full-screen dim.
    rf::gr::set_color(0, 0, 0, 192);
    rf::gr::rect(0, 0, rf::gr::clip_width(), rf::gr::clip_height());

    // Panel background + border.
    rf::gr::set_color(20, 20, 20, 235);
    rf::gr::rect(lo.px, lo.py, lo.pw, lo.ph);
    rf::gr::set_color(120, 120, 120, 255);
    hud_rect_border(lo.px, lo.py, lo.pw, lo.ph, std::max(1, static_cast<int>(2 * lo.scale)));

    // Title doubles as the breadcrumb for the current folder.
    const std::string title_str = g_current_dir.empty() ? std::string{"Demos"} : "Demos\\" + g_current_dir;
    const std::string title = hud_fit_string(title_str, lo.pw - 2 * lo.text_pad, nullptr, title_font);
    rf::gr::set_color(255, 255, 255, 255);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, lo.px + lo.pw / 2,
        lo.py + static_cast<int>(14 * lo.scale), title.c_str(), title_font);

    // Column header (fixed, above the scrolled region).
    const int row_x = lo.content_x + lo.gap;
    rf::gr::set_color(160, 160, 160, 255);
    rf::gr::string(row_x + lo.col_name_x, lo.list_header_y, "Name", font);
    rf::gr::string(row_x + lo.col_level_x, lo.list_header_y, "Level", font);
    rf::gr::string(row_x + lo.col_type_x, lo.list_header_y, "Type", font);
    rf::gr::string(row_x + lo.col_date_x, lo.list_header_y, "Date", font);
    rf::gr::set_color(100, 100, 100, 255);
    rf::gr::rect(lo.content_x, lo.content_y - 1, lo.content_w, 1);

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
        const bool hover = i == hovered && entry.valid;
        const bool previewed = i == g_preview;

        // set_clip moved the drawing origin to (content_x, content_y), so everything drawn while
        // the clip is active must use coords RELATIVE to that origin. The absolute rx/ry above are
        // still what hit-testing (and the cursor) use, so draw at rx-content_x / ry-content_y.
        const int dx = rx - lo.content_x;
        const int dy = ry - lo.content_y;

        rf::gr::set_color(40, 40, 40, hover || previewed ? 255 : 180);
        rf::gr::rect(dx, dy, rw, lo.row_h);
        if (hover || previewed) {
            rf::gr::set_color(255, 255, 255, hover ? 200 : 90);
            hud_rect_border(dx, dy, rw, lo.row_h, std::max(1, static_cast<int>(2 * lo.scale)));
        }

        const int text_y = dy + (lo.row_h - fh) / 2;
        const auto column = [&](int col_x, int col_w, const std::string& text) {
            const std::string fitted = hud_fit_string(text, std::max(20, col_w), nullptr, font);
            rf::gr::string(dx + col_x, text_y, fitted.c_str(), font);
        };
        const int name_w = (entry.kind == Entry::Kind::demo ? lo.col_level_x : lo.col_date_x + lo.col_date_w)
            - lo.col_gap - lo.col_name_x;

        // A trailing backslash marks folder rows (the bitmap font is ASCII-only).
        rf::gr::set_color(entry.valid ? 255 : 140, entry.valid ? 255 : 140, entry.valid ? 255 : 140, 255);
        column(lo.col_name_x, name_w, entry.kind == Entry::Kind::folder ? entry.name + "\\" : entry.name);
        if (entry.kind != Entry::Kind::demo) {
            continue;
        }
        if (!entry.valid) {
            rf::gr::set_color(120, 120, 120, 255);
            column(lo.col_level_x, lo.col_level_w, "unreadable");
            continue;
        }
        draw_level_thumbnail(entry.header.level_filename, dx + lo.thumb_inset, dy + lo.thumb_inset, lo.thumb_w,
            lo.thumb_h);
        rf::gr::set_color(190, 190, 190, 255);
        column(lo.col_level_x, lo.col_level_w, level_display_name(entry.header.level_filename));
        column(lo.col_type_x, lo.col_type_w, entry.type_short);
        column(lo.col_date_x, lo.col_date_w, entry.date_str.empty() ? std::string{"-"} : entry.date_str);
    }

    // Restore the caller's clip window before drawing chrome outside the content region.
    rf::gr::set_clip(save_cx, save_cy, save_cw, save_ch);

    // Scrollbar if the list overflows the content region.
    const float ms = max_scroll(lo);
    if (ms > 0.0f) {
        const float ratio = g_scroll / ms;
        const float bar_h = static_cast<float>(lo.content_h) * lo.content_h / lo.total_list_h;
        const float bar_y = lo.content_y + ratio * (lo.content_h - bar_h);
        const int bar_x = lo.content_x + lo.content_w - lo.bar_w;
        rf::gr::set_color(140, 200, 160, 255);
        rf::gr::rect(bar_x, std::lround(bar_y), lo.bar_w, std::lround(bar_h));
    }

    const bool hovered_demo = hovered >= 0 && g_entries[hovered].kind == Entry::Kind::demo && g_entries[hovered].valid;
    update_preview(hovered_demo ? hovered : -1);
    render_preview(lo, font, fh);

    draw_button(lo.back_x, lo.back_y, lo.back_w, lo.back_h, "Back (Esc)", title_font, title_fh);
    rf::gr::set_color(130, 130, 130, 255);
    rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, lo.pane_x + lo.pane_w, lo.back_y + (lo.back_h - fh) / 2,
        "Click a demo to watch", font);
}

void demo_browser_apply_patch()
{
    extras_init_demos_button_injection.install();
    extras_render_demos_btn_x_injection.install();
    extras_handle_mouse_move_hook.install();
    extras_handle_key_hook.install();
}
