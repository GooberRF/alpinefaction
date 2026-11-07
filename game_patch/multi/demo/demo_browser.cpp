#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <windows.h>
#include <xlog/xlog.h>
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
#include "../../misc/misc.h"
#include "../../rf/gameseq.h"
#include "../../rf/gr/gr.h"
#include "../../rf/gr/gr_font.h"
#include "../../rf/input.h"
#include "../../rf/ui.h"
#include "../../ui/menu_shell.h"
#include "../../ui/settings_skin.h"
#include "../../ui/ui_kit.h"

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
        bool requires_newer = false;
        DemoHeaderInfo header;
        // Needs a full scan of the file, so it is filled in the background for previewed rows
        std::optional<DemoDetails> details;
    };

    struct ScanJob
    {
        std::string name;
        int entry = -1;
        DemoDetails details;
        std::atomic<bool> done = false;
    };

    std::vector<Entry> g_entries;
    bool g_open = false;
    float g_scroll = 0.0f;
    int g_cursor_x = 0;
    int g_cursor_y = 0;
    // Current folder relative to <rf_root>\demos ("" = root); preserved across close/reopen
    // so the browser returns to the folder a demo was launched from.
    std::string g_current_dir;

    // Demo row shown in the preview pane: the overlay's last hovered row, so the pane keeps its
    // content while the cursor leaves the list, or the panel's selection. -1 = none.
    int g_preview = -1;
    std::chrono::steady_clock::time_point g_preview_since;
    // The hover must rest this long before the demo is scanned, so sweeping the cursor down the
    // list doesn't scan every demo it crosses.
    constexpr auto preview_scan_delay = std::chrono::milliseconds{150};
    // Dropped whenever g_entries is rebuilt, so a scan still running can't fill the wrong row
    std::shared_ptr<ScanJob> g_scan_job;

    enum class PanelHit
    {
        none,
        click,
        key,
    };

    // Row the Extras panel's hit-test pass activated, and its PLAY button, run once the pass is
    // over. -1 = none.
    int g_panel_pending = -1;
    PanelHit g_panel_pending_hit = PanelHit::none;
    bool g_panel_play = false;
    // The last click on a demo row, for telling a double-click from two single ones.
    int g_last_click_row = -1;
    std::chrono::steady_clock::time_point g_last_click_time;
    // The second click of a double-click on a folder would land on the folder it opened.
    std::chrono::steady_clock::time_point g_last_nav_click_time;
    UiId g_panel_scrolled_to_focus = 0;

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

    // Width of `sample` with each '0' replaced by the widest digit: the menu font's digits are
    // proportional, so a sample like all zeros can come out narrower than a real value.
    int digit_sample_width(std::string sample, int font)
    {
        static int cached_font = -1;
        static char widest = '0';
        if (font != cached_font) {
            widest = '0';
            for (char digit = '1'; digit <= '9'; ++digit) {
                if (text_width({&digit, 1}, font) > text_width({&widest, 1}, font)) {
                    widest = digit;
                }
            }
            cached_font = font;
        }
        std::replace(sample.begin(), sample.end(), '0', widest);
        return text_width(sample, font);
    }

    int date_column_width(int font)
    {
        return digit_sample_width("0000-00-00 00:00", font);
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
        g_scan_job.reset();
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

    void update_preview(int row)
    {
        const auto now = std::chrono::steady_clock::now();
        if (row >= 0 && row != g_preview) {
            g_preview = row;
            g_preview_since = now;
        }
        if (g_scan_job && g_scan_job->done.load(std::memory_order_acquire)) {
            if (g_scan_job->entry < static_cast<int>(g_entries.size())) {
                g_entries[g_scan_job->entry].details = std::move(g_scan_job->details);
            }
            g_scan_job.reset();
        }
        if (g_scan_job || g_preview < 0 || g_preview >= static_cast<int>(g_entries.size())) {
            return;
        }
        const Entry& entry = g_entries[g_preview];
        if (!entry.details && now - g_preview_since >= preview_scan_delay) {
            auto job = std::make_shared<ScanJob>();
            job->name = join_rel(g_current_dir, entry.name);
            job->entry = g_preview;
            // Detached so a slow disk never blocks the game; the job is shared with the thread
            std::thread{[job]() {
                try {
                    job->details = demo_scan_details(job->name);
                }
                catch (const std::exception& e) {
                    xlog::warn("[DemoBrowser] failed to scan {}: {}", job->name, e.what());
                }
                job->done.store(true, std::memory_order_release);
            }}.detach();
            g_scan_job = std::move(job);
        }
    }

    // Closed but still listed, so the Extras panel can slide out with its rows; leaving the Extras
    // menu releases them (demo_browser_render).
    void close_keeping_entries()
    {
        g_open = false;
        g_panel_pending = -1;
        g_panel_play = false;
        g_scan_job.reset();
    }

    void demo_browser_close()
    {
        close_keeping_entries();
        g_scroll = 0.0f;
        g_preview = -1;
        g_entries.clear();
        map_thumbnails_release_bitmaps();
    }

    // Opening a folder reloads the list, which invalidates every row, so this never runs while a
    // pass is laying the list out.
    void activate_entry(int i)
    {
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
            // On failure the wrapper prints the reason; keep the browser open
            if (entry.valid && !entry.requires_newer
                && demo_playback_start_from_menu(join_rel(g_current_dir, entry.name))) {
                close_keeping_entries();
            }
            break;
        }
    }

    void play_selected()
    {
        if (g_preview < 0 || g_preview >= static_cast<int>(g_entries.size())) {
            return;
        }
        if (g_entries[g_preview].requires_newer) {
            ui_play_error();
            return;
        }
        ui_play_click();
        activate_entry(g_preview);
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
        if (row >= 0) {
            activate_entry(row);
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

    void draw_level_thumbnail(const std::string& level_filename, int x, int y, int w, int h)
    {
        int img_w = 0, img_h = 0;
        const int bm = map_thumbnail_get(level_filename, &img_w, &img_h);
        if (bm == -1 || img_w <= 0 || img_h <= 0) {
            rf::gr::set_color(10, 10, 10, 255);
            rf::gr::rect(x, y, w, h);
            return;
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
    }

    struct PreviewStyle
    {
        int text_pad;
        int gap;
        int col_gap;
        int line_gap;
        int max_thumb_h;
        const char* empty_hint; // shown until a demo is previewed
        rf::Color label;
        rf::Color value;
        rf::Color secondary;
        rf::Color hint;
        rf::Color status;
        rf::Color rule;
        rf::Color notice;
    };

    // The previewed demo's thumbnail and info lines inside `pane`; the caller paints the pane's
    // backing. Returns where its scoreboard starts, once the demo's details are in.
    std::optional<int> render_preview_info(const UiRect& pane, const PreviewStyle& st, int font)
    {
        const int fh = rf::gr::get_font_height(font);
        const int x = pane.x + st.text_pad;
        const int w = pane.w - 2 * st.text_pad;

        if (g_preview < 0 || g_preview >= static_cast<int>(g_entries.size())) {
            rf::gr::set_color(st.hint);
            const std::string hint = hud_fit_string(st.empty_hint, w, nullptr, font);
            rf::gr::string_aligned(rf::gr::ALIGN_CENTER, x + w / 2, pane.y + pane.h / 2 - fh / 2,
                hint.c_str(), font);
            return std::nullopt;
        }
        const Entry& entry = g_entries[g_preview];
        const DemoHeaderInfo& hdr = entry.header;
        int y = pane.y + st.text_pad;

        const int thumb_h = std::min(w * 9 / 16, st.max_thumb_h);
        draw_level_thumbnail(hdr.level_filename, x, y, w, thumb_h);
        const int caption_h = fh + 2 * st.gap;
        const int caption_y = y + thumb_h - caption_h;
        rf::gr::set_color(0, 0, 0, 170);
        rf::gr::rect(x, caption_y, w, caption_h);
        int type_w = 0;
        const std::string type_str = hud_fit_string(entry.type_name, w / 2, &type_w, font);
        rf::gr::set_color(st.secondary);
        rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, x + w - st.text_pad, caption_y + st.gap, type_str.c_str(), font);
        const std::string level_str = hud_fit_string(level_display_name(hdr.level_filename),
            w - type_w - 3 * st.text_pad, nullptr, font);
        rf::gr::set_color(st.value);
        rf::gr::string(x + st.text_pad, caption_y + st.gap, level_str.c_str(), font);
        y += thumb_h + 2 * st.gap;

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
        const int line_h = fh + st.line_gap;
        const int label0_w =
            std::max({text_width("Date:", font), text_width("Players:", font), text_width("Server:", font)})
            + st.text_pad;
        const int label1_w = std::max(text_width("Duration:", font), text_width("Mod:", font)) + st.text_pad;
        const int date_need = label0_w + date_column_width(font) + st.col_gap;
        const int col1_need = label1_w + digit_sample_width("0:00:00 (incomplete)", font);
        const std::string date = entry.date_str.empty() ? std::string{"-"} : entry.date_str;
        const std::string server = hdr.server_name.empty() ? std::string{"-"} : hdr.server_name;
        const auto info = [&](int cx, int line, int label_w, const char* label, const std::string& value,
            int value_w) {
            const int cy = y + line * line_h;
            rf::gr::set_color(st.label);
            rf::gr::string(cx, cy, label, font);
            rf::gr::set_color(st.value);
            const std::string fitted = hud_fit_string(value, std::max(20, value_w), nullptr, font);
            rf::gr::string(cx + label_w, cy, fitted.c_str(), font);
        };
        int info_lines = 0;
        if (date_need + col1_need <= w) {
            const int col0_w = std::min(std::max(w / 2, date_need), w - col1_need);
            const int col1_x = x + col0_w;
            info(x, 0, label0_w, "Date:", date, col0_w - label0_w - st.col_gap);
            info(col1_x, 0, label1_w, "Duration:", duration, w - col0_w - label1_w);
            info(x, 1, label0_w, "Players:", players, col0_w - label0_w - st.col_gap);
            if (!hdr.mod_name.empty()) {
                info(col1_x, 1, label1_w, "Mod:", hdr.mod_name, w - col0_w - label1_w);
            }
            info(x, 2, label0_w, "Server:", server, w - label0_w);
            info_lines = 3;
        }
        else {
            // Too narrow for a full date and duration side by side
            const int label_w = std::max(label0_w, label1_w);
            info(x, info_lines++, label_w, "Date:", date, w - label_w);
            info(x, info_lines++, label_w, "Duration:", duration, w - label_w);
            info(x, info_lines++, label_w, "Players:", players, w - label_w);
            if (!hdr.mod_name.empty()) {
                info(x, info_lines++, label_w, "Mod:", hdr.mod_name, w - label_w);
            }
            info(x, info_lines++, label_w, "Server:", server, w - label_w);
        }
        if (entry.requires_newer) {
            rf::gr::set_color(st.notice);
            const std::string notice = hud_fit_string("Requires a newer Alpine Faction to play", w, nullptr, font);
            rf::gr::string(x, y + info_lines * line_h, notice.c_str(), font);
            ++info_lines;
        }
        y += info_lines * line_h + st.gap;

        if (!details) {
            return std::nullopt;
        }
        if (!details->readable || details->rows.empty()) {
            rf::gr::set_color(st.status);
            rf::gr::string(x, y, details->readable ? "No players recorded" : "Could not read demo file", font);
            return std::nullopt;
        }
        return y;
    }

    // Right edges of the scoreboard's number columns, and the room left for the name.
    struct ScoreColumns
    {
        bool show_caps;
        int score_r, caps_r, ping_r;
        int name_max_w;
    };

    ScoreColumns score_columns(int x, int w, int game_type, const PreviewStyle& st, int font)
    {
        ScoreColumns cols{};
        cols.show_caps = game_type == rf::NG_TYPE_CTF || game_type == rf::NG_TYPE_SAL || game_type == rf::NG_TYPE_SCAV;
        cols.ping_r = x + w;
        cols.caps_r = cols.ping_r - text_width("Ping", font) - st.col_gap;
        cols.score_r =
            (cols.show_caps ? cols.caps_r - text_width("Caps", font) : cols.ping_r - text_width("Ping", font))
            - st.col_gap;
        cols.name_max_w = cols.score_r - text_width("Score", font) - st.col_gap - x;
        return cols;
    }

    // The headings line plus the rule under it.
    int score_header_height(const PreviewStyle& st, int font)
    {
        return rf::gr::get_font_height(font) + 2 * (st.gap / 2) + 1;
    }

    // Returns where the first line goes.
    int draw_score_header(int x, int w, int y, const ScoreColumns& cols, const PreviewStyle& st, int font)
    {
        rf::gr::set_color(st.label);
        rf::gr::string(x, y, "Player", font);
        rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, cols.score_r, y, "Score", font);
        if (cols.show_caps) {
            rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, cols.caps_r, y, "Caps", font);
        }
        rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, cols.ping_r, y, "Ping", font);
        rf::gr::set_color(st.rule);
        rf::gr::rect(x, y + rf::gr::get_font_height(font) + st.gap / 2, w, 1);
        return y + score_header_height(st, font);
    }

    // One scoreboard line; `dx` moves the columns into a scroll region's draw space.
    void draw_score_line(int x, int dx, int y, const ScoreColumns& cols, const ScoreLine& line,
        const DemoDetails& details, const PreviewStyle& st, int font)
    {
        if (line.kind != ScoreLine::Kind::player) {
            std::string label;
            if (line.kind == ScoreLine::Kind::red_header) {
                rf::gr::set_color(255, 90, 90, 255);
                label = details.team_scores_known ? "RED TEAM - " + std::to_string(details.red_score) : "RED TEAM";
            }
            else if (line.kind == ScoreLine::Kind::blue_header) {
                rf::gr::set_color(100, 140, 255, 255);
                label = details.team_scores_known ? "BLUE TEAM - " + std::to_string(details.blue_score)
                                                  : "BLUE TEAM";
            }
            else {
                rf::gr::set_color(st.status);
                label = "OTHER";
            }
            rf::gr::string(x + dx, y, label.c_str(), font);
            return;
        }
        const DemoScoreRow& row = details.rows[line.row];
        rf::gr::set_color(st.value);
        const std::string name = hud_fit_string(row.name, std::max(20, cols.name_max_w), nullptr, font);
        rf::gr::string(x + dx, y, name.c_str(), font);
        rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, cols.score_r + dx, y, std::to_string(row.score).c_str(), font);
        rf::gr::set_color(st.secondary);
        if (cols.show_caps) {
            rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, cols.caps_r + dx, y,
                row.has_stats ? std::to_string(row.caps).c_str() : "-", font);
        }
        rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, cols.ping_r + dx, y,
            row.has_stats ? std::to_string(row.ping).c_str() : "-", font);
    }

    // The overlay's preview: a scoreboard too long for the pane ends in a "+N more" line.
    void render_preview(const Layout& lo, int font)
    {
        const UiRect pane{lo.pane_x, lo.pane_y, lo.pane_w, lo.pane_h};
        rf::gr::set_color(30, 30, 30, 235);
        rf::gr::rect(pane.x, pane.y, pane.w, pane.h);
        rf::gr::set_color(80, 80, 80, 255);
        hud_rect_border(pane.x, pane.y, pane.w, pane.h, 1);
        const PreviewStyle st{
            .text_pad = lo.text_pad,
            .gap = lo.gap,
            .col_gap = lo.col_gap,
            .line_gap = std::max(2, static_cast<int>(6 * lo.scale)),
            .max_thumb_h = pane.h,
            .empty_hint = "Hover a demo to see its details",
            .label = {160, 160, 160, 255},
            .value = {255, 255, 255, 255},
            .secondary = {200, 200, 200, 255},
            .hint = {150, 150, 150, 255},
            .status = {170, 170, 170, 255},
            .rule = {100, 100, 100, 255},
            .notice = {255, 170, 80, 255},
        };

        const std::optional<int> board_y = render_preview_info(pane, st, font);
        if (!board_y) {
            return;
        }
        const DemoDetails& details = *g_entries[g_preview].details;
        const int x = pane.x + st.text_pad;
        const int w = pane.w - 2 * st.text_pad;
        const int bottom = pane.y + pane.h - st.text_pad;
        const int line_h = rf::gr::get_font_height(font) + st.line_gap;
        const ScoreColumns cols = score_columns(x, w, details.header.game_type, st, font);
        int y = draw_score_header(x, w, *board_y, cols, st, font);

        const std::vector<ScoreLine> lines = build_score_lines(details);
        const int max_lines = std::max(0, (bottom - y) / line_h);
        const bool overflow = static_cast<int>(lines.size()) > max_lines;
        const int shown = overflow ? std::max(0, max_lines - 1) : static_cast<int>(lines.size());
        for (int i = 0; i < shown; ++i, y += line_h) {
            draw_score_line(x, 0, y, cols, lines[i], details, st, font);
        }
        if (overflow && max_lines > 0) {
            const int more_players = static_cast<int>(std::count_if(lines.begin() + shown, lines.end(),
                [](const ScoreLine& line) { return line.kind == ScoreLine::Kind::player; }));
            rf::gr::set_color(st.hint);
            rf::gr::string(x, y, ("+" + std::to_string(more_players) + " more").c_str(), font);
        }
    }

    // The Extras menu's Demos panel: the same list and preview, in the menus' skin.

    struct PanelFonts
    {
        int heading;
        int body;
        int body_bold;
        int caption;
    };

    PanelFonts panel_fonts(const UiContext& ctx)
    {
        const float scale = ctx.scale();
        return {menu_font(true, 32.0f, scale), menu_font(false, 20.0f, scale), menu_font(true, 20.0f, scale),
                menu_font(false, 17.0f, scale)};
    }

    // The scoreboard's scroll region as the last draw pass laid it out. Only the draw pass knows
    // where the info lines above it end, so the hit-test pass scrolls by the previous frame's.
    UiRect g_score_view{};
    int g_score_content_h = 0;
    int g_score_step = 0;
    float g_score_scroll = 0.0f;
    // The preview the offset belongs to; another demo starts at the top.
    int g_score_preview = -1;

    void build_panel_preview(UiContext& ctx, const UiRect& pane, int font)
    {
        const auto s = [&](float v) { return skin_px(ctx, v); };
        const UiId id = ui_id("demo_scores");
        if (!ctx.draw) {
            if (g_score_preview == g_preview && g_score_view.h > 0) {
                UiScrollRegion region =
                    ui_scroll_begin(ctx, g_score_view, g_score_content_h, g_score_scroll, g_score_step, id, true);
                ui_scroll_end(ctx, region);
            }
            return;
        }

        if (g_score_preview != g_preview) {
            g_score_preview = g_preview;
            g_score_scroll = 0.0f;
        }
        g_score_view = UiRect{};
        skin_slot(ctx, pane.x, pane.y, pane.w, pane.h);
        const PreviewStyle st{
            .text_pad = s(12.0f),
            .gap = s(6.0f),
            .col_gap = s(16.0f),
            .line_gap = s(6.0f),
            .max_thumb_h = pane.h / 3,
            .empty_hint = "Click a demo to see its details",
            .label = skin::txt_hint,
            .value = skin::txt_hi,
            .secondary = skin::txt,
            .hint = skin::txt_hint,
            .status = skin::txt,
            .rule = skin::border_dim,
            .notice = skin::warn,
        };
        const std::optional<int> board_y = render_preview_info(pane, st, font);
        if (!board_y) {
            return;
        }
        const DemoDetails& details = *g_entries[g_preview].details;
        const std::vector<ScoreLine> lines = build_score_lines(details);
        const int line_h = rf::gr::get_font_height(font) + st.line_gap;
        const int content_h = static_cast<int>(lines.size()) * line_h;
        const int x = pane.x + st.text_pad;
        const int lines_y = *board_y + score_header_height(st, font);
        const UiRect view{pane.x + s(4.0f), lines_y, pane.w - s(8.0f), pane.y + pane.h - s(4.0f) - lines_y};
        // The columns make way for the scrollbar only when there is one.
        const int gutter = content_h > view.h
            ? std::max(ctx.style.scrollbar_w_min, ctx.scaled(ctx.style.scrollbar_w)) + s(4.0f)
            : 0;
        const int w = pane.w - 2 * st.text_pad - gutter;
        const ScoreColumns cols = score_columns(x, w, details.header.game_type, st, font);
        draw_score_header(x, w, *board_y, cols, st, font);
        if (view.h <= 0) {
            return;
        }

        UiScrollRegion region = ui_scroll_begin(ctx, view, content_h, g_score_scroll, line_h, id, true);
        for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
            const int y = region.start_y() + i * line_h;
            if (y + line_h < view.y || y > view.y + view.h) {
                continue;
            }
            draw_score_line(x, -ctx.origin_x, y - ctx.origin_y, cols, lines[i], details, st, font);
        }
        ui_scroll_end(ctx, region);
        skin_scrollbar(ctx, view, content_h, g_score_scroll, ctx.captured_id() == id);

        g_score_view = view;
        g_score_content_h = content_h;
        g_score_step = line_h;
    }

    PanelHit panel_row(UiContext& ctx, const UiRect& r, const Entry& entry, bool selected,
        const PanelFonts& fonts, UiId id)
    {
        const bool hovered = ui_hover(ctx, r, id);
        if (hovered) {
            ctx.note_hovered(id);
        }
        ctx.register_focusable(id, UiFocusKind::Activate);
        if (!ctx.draw) {
            if (ctx.consume_activation(id)) {
                return PanelHit::key;
            }
            return hovered && ctx.click ? PanelHit::click : PanelHit::none;
        }

        const auto s = [&](float v) { return skin_px(ctx, v); };
        const UiRect d = ui_draw_rect(ctx, r);
        rf::gr::set_color(skin::border_dim);
        rf::gr::rect(d.x + s(8.0f), d.y + d.h - s(1.0f), d.w - s(8.0f), s(1.0f));
        if (selected) {
            const int inset = s(2.0f);
            skin_selection_bar(ctx, d.x, d.y + inset, d.w, d.h - 2 * inset);
        }
        const bool lit = hovered || selected || (ctx.focus_visible() && ctx.focus_id() == id);
        const rf::Color main_color = !entry.valid ? skin::txt_dim : lit ? skin::txt_hi : skin::txt;
        const rf::Color sub_color = !entry.valid ? skin::txt_dim : lit ? skin::txt : skin::txt_hint;

        // Clear of the selection bar's LED.
        const int thumb_x = d.x + s(24.0f);
        const int thumb_inset = s(5.0f);
        const int thumb_h = d.h - 2 * thumb_inset;
        const int thumb_w = thumb_h * 16 / 9;
        const int text_x = thumb_x + thumb_w + s(12.0f);
        const int right = d.x + d.w - s(12.0f);
        const int body_h = rf::gr::get_font_height(fonts.body);
        const int caption_h = rf::gr::get_font_height(fonts.caption);

        if (entry.kind != Entry::Kind::demo) {
            // A trailing backslash marks folder rows, as in the overlay.
            const std::string label = entry.kind == Entry::Kind::folder ? entry.name + "\\" : entry.name;
            rf::gr::set_color(main_color);
            rf::gr::string(text_x, d.y + (d.h - body_h) / 2,
                ui_fit_middle(label, std::max(20, right - text_x), fonts.body).c_str(), fonts.body);
            return PanelHit::none;
        }

        const int top = d.y + (d.h - (body_h + s(2.0f) + caption_h)) / 2;
        const int sub_y = top + body_h + s(2.0f);
        int stamp_w = 0;
        if (entry.valid) {
            draw_level_thumbnail(entry.header.level_filename, thumb_x, d.y + thumb_inset, thumb_w, thumb_h);
            // Sized from the widest digits so every row's name stops at the same place.
            const std::size_t space = entry.date_str.find(' ');
            const std::string date = entry.date_str.empty() ? std::string{"-"} : entry.date_str.substr(0, space);
            const std::string time = space == std::string::npos ? std::string{} : entry.date_str.substr(space + 1);
            stamp_w = digit_sample_width("0000-00-00", fonts.caption) + s(16.0f);
            rf::gr::set_color(sub_color);
            rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, right, top + (body_h - caption_h) / 2, date.c_str(),
                fonts.caption);
            rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, right, sub_y, time.c_str(), fonts.caption);
        }
        const int text_w = std::max(20, right - stamp_w - text_x);

        rf::gr::set_color(main_color);
        // From the middle: an auto-recorded name ends in the suffix that tells same-map demos apart.
        rf::gr::string(text_x, top, ui_fit_middle(entry.name, text_w, fonts.body).c_str(), fonts.body);
        rf::gr::set_color(sub_color);
        if (!entry.valid) {
            rf::gr::string(text_x, sub_y, "unreadable", fonts.caption);
            return PanelHit::none;
        }
        // Fitted from the end, so the level name gives way before the game type does.
        const std::string sub = entry.type_short + ": " + level_display_name(entry.header.level_filename);
        rf::gr::string(text_x, sub_y, hud_fit_string(sub, text_w, nullptr, fonts.caption).c_str(), fonts.caption);
        return PanelHit::none;
    }

    void build_panel_list(UiContext& ctx, const UiRect& well, const PanelFonts& fonts)
    {
        const auto s = [&](float v) { return skin_px(ctx, v); };
        const UiRect view{well.x + s(4.0f), well.y + s(4.0f), well.w - s(8.0f), well.h - s(8.0f)};
        // The bitmap fonts used below 480 px do not shrink with the scale.
        const int row_h = std::max(s(64.0f),
            rf::gr::get_font_height(fonts.body) + rf::gr::get_font_height(fonts.caption) + s(12.0f));
        const int gutter = std::max(ctx.style.scrollbar_w_min, ctx.scaled(ctx.style.scrollbar_w));
        const int row_w = view.w - gutter - s(4.0f);

        if (ctx.draw) {
            skin_slot(ctx, well.x, well.y, well.w, well.h);
        }

        const int count = static_cast<int>(g_entries.size());
        const int content_h = count * row_h;
        const UiId list_id = ui_id("demo_list");
        UiScrollRegion region = ui_scroll_begin(ctx, view, content_h, g_scroll, row_h, list_id, true);
        std::optional<UiRect> focus_rect;
        for (int i = 0; i < count; ++i) {
            const UiRect rect{view.x, region.start_y() + i * row_h, row_w, row_h};
            const UiId id = ui_id("demo_row", i);
            if (ctx.focus_id() == id) {
                focus_rect = rect;
            }
            if (rect.y + rect.h < view.y || rect.y > view.y + view.h) {
                // Still reachable by keyboard: the focus can step onto it and scroll it into view.
                ctx.register_focusable(id, UiFocusKind::Activate);
                if (!ctx.draw && ctx.consume_activation(id)) {
                    g_panel_pending = i;
                    g_panel_pending_hit = PanelHit::key;
                }
                continue;
            }
            if (const PanelHit hit = panel_row(ctx, rect, g_entries[i], i == g_preview, fonts, id);
                hit != PanelHit::none) {
                g_panel_pending = i;
                g_panel_pending_hit = hit;
            }
        }

        // The ".." row alone doesn't count as content (it still renders above the message).
        const bool only_up = count == 1 && g_entries[0].kind == Entry::Kind::up;
        if (ctx.draw && (count == 0 || only_up)) {
            const UiRect d = ui_draw_rect(ctx, UiRect{view.x, view.y + (only_up ? row_h : 0), row_w, row_h});
            rf::gr::set_color(skin::txt_hint);
            rf::gr::string_aligned(rf::gr::ALIGN_CENTER, d.x + d.w / 2,
                d.y + (d.h - rf::gr::get_font_height(fonts.body)) / 2,
                g_current_dir.empty() ? "No demos found" : "Folder is empty", fonts.body);
        }

        ui_scroll_end(ctx, region);

        if (ctx.draw) {
            skin_scrollbar(ctx, view, content_h, g_scroll, ctx.captured_id() == list_id);
            return;
        }

        ui_scroll_focus_into_view(ctx, view, focus_rect, g_scroll, g_panel_scrolled_to_focus);

        // The preview follows the selection; this advances its delayed details scan.
        update_preview(-1);
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

void demo_browser_back()
{
    if (!g_current_dir.empty()) {
        navigate_up();
    }
    else {
        close_keeping_entries();
    }
}

void demo_browser_build_panel(UiContext& ctx, const MenuPanel& panel, int canvas_y)
{
    // Truncated like the Options panel's layout, so the shared header lines up with it.
    const auto s = [&](float v) { return ctx.scaled(v); };
    const auto cy = [&](float v) { return canvas_y + ctx.scaled(v); };
    const PanelFonts fonts = panel_fonts(ctx);
    const int x = panel.content_x;
    const int w = panel.content_r - x;
    const UiRect header{x, cy(menu_panel_header_y), w, s(menu_panel_header_h)};
    const int body_y = cy(menu_panel_body_y);
    const int footer_y = panel.inset.y + panel.inset.h - s(menu_panel_footer_inset);
    const int body_h = footer_y - s(16.0f) - body_y;

    if (ctx.draw) {
        // The title doubles as the breadcrumb for the current folder.
        rf::gr::set_color(skin::ink_title);
        const int title_w = text_width("DEMOS", fonts.heading);
        rf::gr::string(header.x, header.y + (header.h - rf::gr::get_font_height(fonts.heading)) / 2, "DEMOS",
            fonts.heading);
        if (!g_current_dir.empty()) {
            const int crumb_x = header.x + title_w + s(16.0f);
            const SkinText crumb =
                skin_fit_text(ctx, "\\" + g_current_dir, fonts.body, std::max(0, header.x + header.w - crumb_x));
            rf::gr::set_color(skin::ink_status);
            rf::gr::string(crumb_x, header.y + (header.h - rf::gr::get_font_height(crumb.font)) / 2,
                crumb.text.c_str(), crumb.font);
        }
        skin_ridge(ctx, panel.inset.x + s(8.0f), header.y + header.h + s(1.0f), panel.inset.w - s(16.0f));
    }

    const int gap = s(16.0f);
    const int pane_w = (w - gap) * 2 / 5;
    build_panel_list(ctx, UiRect{x, body_y, w - gap - pane_w, body_h}, fonts);
    build_panel_preview(ctx, UiRect{x + w - pane_w, body_y, pane_w, body_h}, fonts.caption);

    const int footer_h = s(menu_panel_footer_h);
    const int play_w = skin_button_width(ctx, "PLAY", fonts.body_bold, 140.0f);
    const bool playable = g_preview >= 0 && g_preview < static_cast<int>(g_entries.size())
        && !g_entries[g_preview].requires_newer;
    if (skin_button(ctx, UiRect{x + w - play_w, footer_y, play_w, footer_h}, "PLAY", fonts.body_bold,
            SkinButton::Secondary, playable, ui_id("demo_play"))) {
        g_panel_play = true;
    }
    if (ctx.draw && !g_current_dir.empty()) {
        rf::gr::set_color(skin::ink_status);
        rf::gr::string(x, footer_y + (footer_h - rf::gr::get_font_height(fonts.caption)) / 2,
            "Esc goes up a folder", fonts.caption);
    }
}

void demo_browser_commit_panel()
{
    const int row = std::exchange(g_panel_pending, -1);
    const PanelHit hit = std::exchange(g_panel_pending_hit, PanelHit::none);
    const bool play = std::exchange(g_panel_play, false);
    if (!g_open) {
        return;
    }
    if (play) {
        play_selected();
        return;
    }
    if (row < 0 || row >= static_cast<int>(g_entries.size())) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto double_click_time = std::chrono::milliseconds{GetDoubleClickTime()};
    if (hit == PanelHit::click && now - g_last_nav_click_time <= double_click_time) {
        return;
    }
    const Entry& entry = g_entries[row];
    if (entry.kind != Entry::Kind::demo) {
        if (hit == PanelHit::click) {
            g_last_nav_click_time = now;
        }
        ui_play_click();
        activate_entry(row);
        return;
    }
    if (!entry.valid) {
        ui_play_error();
        return;
    }
    // A click selects a demo; a double-click, or Enter on the selected one, plays it.
    const bool double_click =
        hit == PanelHit::click && row == g_last_click_row && now - g_last_click_time <= double_click_time;
    g_last_click_row = hit == PanelHit::click && !double_click ? row : -1;
    g_last_click_time = now;
    if (row == g_preview && (double_click || hit == PanelHit::key)) {
        play_selected();
        return;
    }
    ui_play_click();
    update_preview(row);
    // The scan delay is for the overlay's hover; a selection is deliberate.
    g_preview_since -= preview_scan_delay;
}

void demo_browser_render()
{
    // Close the browser if the game state changes (playback started, menu left) so it
    // doesn't get stuck open.
    if (rf::gameseq_get_state() != rf::GS_EXTRAS_MENU) {
        if (g_open || !g_entries.empty()) {
            demo_browser_close();
        }
        return;
    }
    // The Alpine Extras menu shows the browser as one of its panels.
    if (!g_open || !legacy_menus_enabled()) {
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
    render_preview(lo, font);

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
