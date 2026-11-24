#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <windows.h>
#include <common/utils/string-utils.h>
#include <xlog/xlog.h>
#include "save_load_menu.h"
#include "column_menu.h"
#include "form_view.h"
#include "menu_popup.h"
#include "menu_shell.h"
#include "settings_skin.h"
#include "ui_kit.h"
#include "../multi/multi.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/input.h"
#include "../rf/localize.h"
#include "../rf/os/console.h"
#include "../rf/os/os.h"
#include "../rf/player/player.h"
#include "../rf/save_restore.h"
#include "../rf/sound/sound.h"
#include "../rf/ui.h"
#include "../savegame/asg.h"
#include "../savegame/asg_format.h"
#include "../sound/sound.h"

namespace
{

// Act and Back are the column's buttons too.
enum class SaveLoadAction
{
    Act,
    Back,
    Delete,
};

constexpr std::chrono::milliseconds double_click_time{400};
// What stock's name box holds, and what it copies of a picked row.
constexpr std::size_t save_name_max_len = 31;
constexpr const char* row_id_name = "saved_game";
constexpr const char* delete_label = "DELETE";
constexpr const char* delete_title = "DELETE SAVED GAME";
const UiId save_name_id = ui_id("save_name");
const UiId act_button_id = ui_id("save_load_act");
const UiId delete_button_id = ui_id("save_load_delete");

// Kept from visit to visit, as stock's box is.
std::string g_save_name = "savegame";
bool g_save = false;

// Stock's save box refuses these, and every stock text box `%` too.
bool save_name_char(char c)
{
    switch (c) {
        case '%':
        case '\\':
        case '/':
        case '*':
        case ':':
        case '"':
        case '|':
        case '<':
        case '>':
        case '?':
            return false;
        default:
            return static_cast<unsigned char>(c) >= 0x20;
    }
}

std::string_view entry_name(const rf::ui::SavegameListEntry& entry)
{
    return {entry.filename, strnlen(entry.filename, sizeof(entry.filename))};
}

// Stock indents its column headers with spaces.
std::string trimmed(std::string_view text)
{
    return std::string{ltrim(text)};
}

// strftime("%c") as stock's runtime prints it.
std::string date_string(std::int64_t write_time)
{
    const std::time_t t = static_cast<std::time_t>(write_time);
    const std::tm* tm = std::localtime(&t);
    if (!tm) {
        return {};
    }
    char buf[32];
    const std::size_t n = std::strftime(buf, sizeof(buf), "%m/%d/%y %H:%M:%S", tm);
    return std::string{buf, n};
}

std::string game_time_string(float t)
{
    const int hours = static_cast<int>(t) / 3600;
    const float rest = t - static_cast<float>(hours * 3600);
    const int minutes = static_cast<int>(rest) / 60;
    const int seconds = static_cast<int>(rest - static_cast<float>(minutes * 60));
    return std::format("{:02}:{:02}:{:02}", hours, minutes, seconds);
}

// A saved game in the list: an .svl from stock's list, or an Alpine .asg.
struct SaveRow
{
    std::string filename; // with its extension
    std::string name;     // as listed: the extension only when a save of the other format has the same name
    std::int64_t write_time = 0;
    float game_time = 0.0f;
    std::string level; // the level's file
    std::string title;
    // An .svl's index in stock's list, which loads it.
    int stock_index = -1;
    std::optional<asg::Header> header; // an .asg's
};

std::vector<SaveRow> g_rows;

int row_count()
{
    return static_cast<int>(g_rows.size());
}

std::int64_t unix_time(const FILETIME& ft)
{
    const std::uint64_t ticks = static_cast<std::uint64_t>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime;
    return static_cast<std::int64_t>(ticks / 10000000ULL) - 11644473600LL;
}

void add_asg_rows()
{
    WIN32_FIND_DATAA data;
    const std::string pattern = std::string{rf::sr::savegame_path} + "*.asg";
    const HANDLE find = FindFirstFileA(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        // The pattern also matches longer extensions through their short names.
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !string_iends_with(data.cFileName, ".asg")) {
            continue;
        }
        asg::Header header;
        if (!asg_read_header(std::filesystem::path{rf::sr::savegame_path} / data.cFileName, header)) {
            continue;
        }
        g_rows.push_back({
            .filename = data.cFileName,
            .write_time = unix_time(data.ftLastWriteTime),
            .game_time = header.game_time,
            .level = header.current_level,
            .title = header.level_title,
            .header = std::move(header),
        });
    } while (FindNextFileA(find, &data));
    FindClose(find);
}

// Stock's builder lists only .svl saves, and its list is kept for loading them.
void rebuild_list()
{
    rf::VArray<rf::ui::SavegameListEntry>& stock = g_save ? rf::ui::save_game_list : rf::ui::load_game_list;
    stock.clear();
    if (g_save) {
        rf::ui::save_game_build_list();
    }
    else {
        rf::ui::load_game_build_list();
    }
    g_rows.clear();
    for (int i = 0; i < stock.size(); ++i) {
        const rf::ui::SavegameListEntry& entry = stock[static_cast<std::size_t>(i)];
        SaveRow& row = g_rows.emplace_back();
        row.filename = entry_name(entry);
        row.write_time = entry.write_time;
        row.game_time = entry.game_time;
        row.stock_index = i;
        char level[260]{};
        char title[260]{};
        const std::string path = std::string{rf::sr::savegame_path} + row.filename;
        if (rf::sr::read_header(path.c_str(), level, nullptr, title)) {
            row.level = level;
            row.title = title;
        }
    }
    add_asg_rows();
    for (SaveRow& row : g_rows) {
        const std::string_view row_stem = get_filename_without_ext(row.filename);
        const bool shared = std::ranges::any_of(g_rows, [&](const SaveRow& other) {
            return &other != &row && string_iequals(get_filename_without_ext(other.filename), row_stem);
        });
        row.name = shared ? row.filename : std::string{row_stem};
    }
    // Newest first, as stock's list.
    std::ranges::stable_sort(g_rows, std::ranges::greater{}, &SaveRow::write_time);
}

void run_action(SaveLoadAction action);
void focus_name_box();

class SavedGamesPanel final : public ColumnPanel
{
public:
    void reset()
    {
        // Load starts on the newest save, as stock's list; Save on none until one is picked.
        sel_ = !g_save && row_count() > 0 ? 0 : -1;
        scroll_ = 0.0f;
        scrolled_to_ = 0;
        scroll_to_sel_ = false;
        last_click_row_ = -1;
        pending_.reset();
        pending_action_.reset();
    }
    [[nodiscard]] int selected() const
    {
        return sel_;
    }
    // After the list is rebuilt. Save's pick goes: a save has emptied its box, a delete has removed its row.
    void clamp_selection()
    {
        const int count = row_count();
        sel_ = g_save || count == 0 ? -1 : std::clamp(sel_, 0, count - 1);
        scroll_to_sel_ = sel_ >= 0;
        last_click_row_ = -1;
    }
    // Tab or Shift+Tab, from the list, the name box or the footer's buttons, or from the column into them.
    void tab(UiContext& ctx, int key);
    // Up or Down: the next or previous row picked.
    void step(UiContext& ctx, int key);

    [[nodiscard]] bool is_open() const override
    {
        return true;
    }
    void open() override {}
    void close() override {}
    bool back() override
    {
        return false;
    }
    void build(UiContext& ctx, const MenuPanel& panel, const MenuFonts& fonts) override;
    void commit() override;
    bool key(UiContext& ctx, int key) override;
    void hints(const UiContext& ctx, std::vector<MenuHint>& hints) const override;

private:
    struct RowHit
    {
        int row = 0;
        bool click = false;
    };

    [[nodiscard]] bool has_selection() const
    {
        return sel_ >= 0 && sel_ < row_count();
    }
    [[nodiscard]] bool can_act() const
    {
        return g_save ? !g_save_name.empty() : has_selection();
    }
    [[nodiscard]] bool row_focused(const UiContext& ctx) const;
    [[nodiscard]] std::vector<UiId> tab_stops() const;
    void select(int row);
    void pick(const RowHit& hit);
    void build_footer(UiContext& ctx, const UiRect& footer, const MenuFonts& fonts);

    int sel_ = -1;
    float scroll_ = 0.0f;
    UiId scrolled_to_ = 0;
    bool scroll_to_sel_ = false;
    int last_click_row_ = -1;
    std::chrono::steady_clock::time_point last_click_{};
    std::optional<RowHit> pending_;
    std::optional<SaveLoadAction> pending_action_;
};

void SavedGamesPanel::select(int row)
{
    sel_ = row;
    if (g_save) {
        const std::string_view name = g_rows[static_cast<std::size_t>(row)].filename;
        g_save_name = std::string{name.substr(0, save_name_max_len)};
    }
}

void SavedGamesPanel::pick(const RowHit& hit)
{
    const auto now = std::chrono::steady_clock::now();
    // As stock: the selected row picked again within 400 ms acts, and Enter on the load list loads at once.
    const bool again = hit.row == sel_ && hit.row == last_click_row_ && now - last_click_ <= double_click_time;
    if (again || (!hit.click && !g_save)) {
        last_click_row_ = -1;
        sel_ = hit.row;
        run_action(SaveLoadAction::Act);
        return;
    }
    select(hit.row);
    last_click_row_ = hit.row;
    last_click_ = now;
    ui_play_click();
    // Stock's box never lets go of the typing, so letters never reach the column's hotkeys. Commit runs after the pass
    // in which the click let go of it.
    if (g_save) {
        focus_name_box();
    }
}

bool SavedGamesPanel::row_focused(const UiContext& ctx) const
{
    const int count = row_count();
    for (int i = 0; i < count; ++i) {
        if (ctx.focus_id() == ui_id(row_id_name, i)) {
            return true;
        }
    }
    return false;
}

// In the order they are built, which keyboard navigation follows. The list is one stop, at its selected row.
std::vector<UiId> SavedGamesPanel::tab_stops() const
{
    std::vector<UiId> stops;
    if (row_count() > 0) {
        stops.push_back(ui_id(row_id_name, std::max(sel_, 0)));
    }
    if (g_save) {
        stops.push_back(save_name_id);
    }
    if (can_act()) {
        stops.push_back(act_button_id);
    }
    if (has_selection()) {
        stops.push_back(delete_button_id);
    }
    return stops;
}

void SavedGamesPanel::tab(UiContext& ctx, int key)
{
    const auto focus_stop = [&](UiId id) {
        ctx.set_focus(id, true);
        // The box takes the typing at once, so letters never reach the column's hotkeys.
        if (id == save_name_id) {
            ctx.focus_text(id);
        }
    };
    const bool back = (key & rf::KEY_SHIFTED) != 0;
    const std::vector<UiId> stops = tab_stops();
    const bool on_list = row_focused(ctx);
    const auto at = std::ranges::find(stops, on_list ? stops.front() : ctx.focus_id());
    if (at != stops.end()) {
        const auto next = (at - stops.begin()) + (back ? -1 : 1);
        if (next >= 0 && next < std::ssize(stops)) {
            focus_stop(stops[static_cast<std::size_t>(next)]);
            return;
        }
        // Off an end of the panel, from the list's own end rather than the next of its rows.
        if (on_list) {
            ctx.set_focus(ui_id(row_id_name, back ? 0 : row_count() - 1), true);
        }
    }
    ctx.handle_nav_key(static_cast<rf::Key>(key));
    if (row_focused(ctx)) {
        focus_stop(stops.front());
    }
    else if (ctx.focus_id() == save_name_id) {
        focus_stop(save_name_id);
    }
}

void SavedGamesPanel::step(UiContext& ctx, int key)
{
    last_click_row_ = -1;
    if (const int count = row_count(); count > 0) {
        const bool follow = row_focused(ctx);
        // The typing not yet applied goes with the name it was typed into.
        ctx.clear_pending_keys();
        select(std::clamp(sel_ + (key == rf::KEY_UP ? -1 : 1), 0, count - 1));
        scroll_to_sel_ = true;
        if (follow) {
            ctx.set_focus(ui_id(row_id_name, sel_), ctx.focus_visible());
        }
    }
    rf::snd_play(key == rf::KEY_UP ? stock_sound_id::list_scroll_up : stock_sound_id::list_scroll_down, 0, 0.0f, 1.0f);
}

bool SavedGamesPanel::key(UiContext& ctx, int key)
{
    // As Enter: on the list, unless the keyboard focus shows on another widget, which the arrows move from.
    const bool focus_off_list = ctx.focus_visible() && ctx.focus_id() != 0 && !row_focused(ctx);
    if (key == rf::KEY_UP || key == rf::KEY_DOWN) {
        if (!focus_off_list) {
            step(ctx, key);
            return true;
        }
        // As Tab, so the list is one stop at its selected row.
        tab(ctx, key == rf::KEY_UP ? rf::KEY_TAB | rf::KEY_SHIFTED : rf::KEY_TAB);
        return true;
    }
    if ((key & rf::KEY_MASK) == rf::KEY_TAB) {
        tab(ctx, key);
        return true;
    }
    if (key == rf::KEY_DELETE) {
        if (has_selection()) {
            pending_action_ = SaveLoadAction::Delete;
        }
        return true;
    }
    // Enter on the list, unless the keyboard focus shows on a widget to press.
    if ((key == rf::KEY_ENTER || key == rf::KEY_PADENTER) && !(ctx.focus_visible() && ctx.focus_id() != 0)) {
        if (has_selection()) {
            pending_ = RowHit{sel_, false};
        }
        return true;
    }
    return false;
}

void SavedGamesPanel::hints(const UiContext& ctx, std::vector<MenuHint>& hints) const
{
    const bool has_rows = row_count() > 0;
    // key_callback's keys for the name box.
    if (ctx.focused_text_id() == save_name_id) {
        hints = {{"ESC", "Back"}};
        if (has_rows) {
            hints.push_back({{}, "Move", MenuHintArrows::UpDown});
        }
        hints.push_back({"ENTER", "Save"});
        hints.push_back({"TAB", "Next field"});
        return;
    }
    if (ctx.focused_text_id() != 0) {
        return;
    }
    // As key(): the list takes the arrows and Enter unless the keyboard focus shows on another widget. A row it shows
    // on takes Enter as pick() does.
    const bool on_list = !ctx.focus_visible() || ctx.focus_id() == 0;
    if (on_list || row_focused(ctx)) {
        std::erase_if(hints, [](const MenuHint& hint) { return hint.key == "ENTER"; });
        if (!on_list || has_selection()) {
            hints.push_back({"ENTER", g_save ? "Pick" : "Load"});
        }
    }
    if (on_list && !has_rows) {
        std::erase_if(hints, [](const MenuHint& hint) { return hint.arrows == MenuHintArrows::UpDown; });
    }
    if (has_selection()) {
        hints.push_back({"DEL", "Delete"});
    }
    hints.push_back({"TAB", "Next field"});
}

void SavedGamesPanel::commit()
{
    // One of them a frame, so neither can open a popup over the other's.
    const std::optional<RowHit> hit = std::exchange(pending_, std::nullopt);
    const std::optional<SaveLoadAction> action = std::exchange(pending_action_, std::nullopt);
    if (hit && hit->row < row_count()) {
        pick(*hit);
    }
    else if (action) {
        run_action(*action);
    }
}

void SavedGamesPanel::build_footer(UiContext& ctx, const UiRect& footer, const MenuFonts& fonts)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const int gap = s(16.0f);
    const std::string act_label = menu_stock_label(g_save ? rf::strings::save : rf::strings::load);
    const int delete_w = skin_button_width(ctx, delete_label, fonts.column, 150.0f);
    const int act_w = skin_button_width(ctx, act_label.c_str(), fonts.column, 150.0f);
    const UiRect delete_rect{footer.x + footer.w - delete_w, footer.y, delete_w, footer.h};
    const UiRect act_rect{delete_rect.x - gap - act_w, footer.y, act_w, footer.h};

    if (g_save) {
        const std::string label = trimmed(rf::strings::name_column);
        const int label_w = rf::gr::get_string_size(label, fonts.column).first;
        const int field_x = footer.x + label_w + gap;
        const UiRect field{field_x, footer.y, std::max(0, act_rect.x - gap - field_x), footer.h};
        if (ctx.draw) {
            rf::gr::set_color(skin::ink_title);
            rf::gr::string(footer.x, footer.y + (footer.h - rf::gr::get_font_height(fonts.column)) / 2,
                           label.c_str(), fonts.column);
        }
        if (skin_field(ctx, field, save_name_id, g_save_name, save_name_max_len, save_name_char, "", fonts.body,
                       true)) {
            // As stock's box: a space only after a character.
            g_save_name.erase(0, std::min(g_save_name.find_first_not_of(' '), g_save_name.size()));
        }
    }

    if (skin_button(ctx, act_rect, act_label.c_str(), fonts.column, SkinButton::Secondary, can_act(), act_button_id)) {
        pending_action_ = SaveLoadAction::Act;
    }
    if (skin_button(ctx, delete_rect, delete_label, fonts.column, SkinButton::Secondary, has_selection(),
                    delete_button_id)) {
        pending_action_ = SaveLoadAction::Delete;
    }
}

// The picked save's level preview and details, as the demo browser's pane.
void draw_preview(const UiContext& ctx, const UiRect& pane, const SaveRow* row, int font)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const int pad = s(12.0f);
    const int gap = s(6.0f);
    const int fh = rf::gr::get_font_height(font);
    const int x = pane.x + pad;
    const int w = pane.w - 2 * pad;
    skin_slot(ctx, pane.x, pane.y, pane.w, pane.h);
    if (!row) {
        rf::gr::set_color(skin::txt_hint);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, x + w / 2, pane.y + (pane.h - fh) / 2,
                               ui_fit_end("Pick a save to see its details.", w, font).c_str(), font);
        return;
    }

    static constexpr const char* difficulty_names[] = {"Easy", "Medium", "Hard", "Impossible"};
    std::vector<std::pair<const char*, std::string>> lines;
    lines.emplace_back("Level:", row->level.empty() ? "-" : row->level);
    lines.emplace_back("Saved:", date_string(row->write_time));
    lines.emplace_back("Game time:", game_time_string(row->game_time));
    bool other_mod = false;
    if (row->header) {
        lines.emplace_back("Difficulty:", difficulty_names[std::min<int>(row->header->difficulty, 3)]);
        if (!row->header->mod_name.empty()) {
            lines.emplace_back("Mod:", row->header->mod_name);
        }
        // asg_load_game refuses it.
        other_mod = !string_iequals(row->header->mod_name, asg_running_mod_name());
    }
    const int line_h = fh + gap;
    const int text_h = (static_cast<int>(lines.size()) + (other_mod ? 1 : 0)) * line_h;

    // The thumbnail gives way to the details in a short pane; with no level, the caption band alone.
    const int cap_h = fh + 2 * gap;
    const int thumb_h = row->level.empty()
        ? cap_h
        : std::max(cap_h, std::min(w * 9 / 16, pane.h - 2 * pad - 2 * gap - text_h));
    const std::string title = row->title.empty() ? level_filename_without_ext(row->level) : row->title;
    const MenuLevelCaption caption{
        .level = title,
        .type = row->header ? "Alpine" : "Stock",
        .font = font,
        .cap_h = cap_h,
        .pad = pad,
        .level_color = skin::txt_hi,
        .type_color = skin::txt,
    };
    int y = pane.y + pad;
    menu_draw_level_preview(row->level, UiRect{x, y, w, thumb_h}, caption);
    y += thumb_h + 2 * gap;

    int label_w = 0;
    for (const auto& [label, value] : lines) {
        label_w = std::max(label_w, rf::gr::get_string_size(label, font).first);
    }
    label_w += pad;
    for (const auto& [label, value] : lines) {
        rf::gr::set_color(skin::txt_hint);
        rf::gr::string(x, y, label, font);
        skin_draw_text(x + label_w, y, ui_fit_end(value, std::max(1, w - label_w), font), font, skin::txt_hi);
        y += line_h;
    }
    if (other_mod) {
        skin_draw_text(x, y, ui_fit_end("Saved with another mod", w, font), font, skin::warn);
    }
}

void SavedGamesPanel::build(UiContext& ctx, const MenuPanel& panel, const MenuFonts& fonts)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const UiRect& body = panel.body;
    const UiRect footer = form_panel_areas(ctx, panel, false).footer;
    const int hint_y = footer.y - s(10.0f) - rf::gr::get_font_height(fonts.caption);
    // The list, and the preview beside it as the demo browser's.
    const int area_h = hint_y - s(10.0f) - body.y;
    const int pane_gap = s(16.0f);
    const int pane_w = (body.w - pane_gap) * 2 / 5;
    const UiRect well{body.x, body.y, body.w - pane_gap - pane_w, area_h};
    const UiRect pane{body.x + body.w - pane_w, body.y, pane_w, area_h};
    const int row_h = skin_list_row_h(ctx, fonts.body);

    // The text columns, from where skin_scroll_list puts its rows in the well. The game time is never cut, as stock
    // sizes its column to hold it; the date is in the preview, which leaves the name the rest of the row.
    const int row_x = well.x + s(4.0f);
    const int row_w = well.w - s(8.0f) - ui_scrollbar_width(ctx) - s(4.0f);
    const int name_x = s(24.0f);
    const int gap = s(12.0f);
    const int time_w = std::max(rf::gr::get_string_size("00:00:00", fonts.body).first,
                                rf::gr::get_string_size(trimmed(rf::strings::game_time_column), fonts.column).first)
                     + gap;
    const int time_x = row_w - time_w;

    if (ctx.draw) {
        const UiRect& header = panel.header;
        const int y = header.y + (header.h - rf::gr::get_font_height(fonts.column)) / 2;
        rf::gr::set_color(skin::ink_title);
        rf::gr::string(row_x + name_x, y, trimmed(rf::strings::name_column).c_str(), fonts.column);
        rf::gr::string(row_x + time_x, y, trimmed(rf::strings::game_time_column).c_str(), fonts.column);
        menu_draw_panel_ridge(ctx, panel);
    }
    else if (std::exchange(scroll_to_sel_, false) && sel_ >= 0) {
        const float top = static_cast<float>(sel_ * row_h);
        const float view_h = static_cast<float>(well.h - s(8.0f));
        if (top < scroll_) {
            scroll_ = top;
        }
        else if (top + static_cast<float>(row_h) > scroll_ + view_h) {
            scroll_ = top + static_cast<float>(row_h) - view_h;
        }
    }

    const SkinScrollList scroll_list{
        .count = row_count(),
        .row_h = row_h,
        .id_name = "saved_games",
        .row_id_name = row_id_name,
        .empty_text = g_rows.empty() ? "No saved games." : nullptr,
        .font = fonts.body,
    };
    skin_scroll_list(ctx, well, scroll_list, scroll_, scrolled_to_, [&](const SkinScrollListRow& row) {
        if (!row.in_view) {
            if (row.offscreen_activated) {
                pending_ = RowHit{row.index, false};
            }
            return;
        }
        const SkinListRow r = skin_list_row(ctx, row.rect, row.index == sel_, row.id);
        if (!ctx.draw) {
            if (r.clicked || r.activated) {
                pending_ = RowHit{row.index, r.clicked};
            }
            return;
        }
        const SaveRow& save = g_rows[static_cast<std::size_t>(row.index)];
        const rf::Color color = r.lit ? skin::txt_hi : skin::txt;
        const int text_y = r.draw.y + (r.draw.h - rf::gr::get_font_height(fonts.body)) / 2;
        const auto cell = [&](int x, int right, std::string_view text) {
            skin_draw_text(r.draw.x + x, text_y, ui_fit_end(text, std::max(1, right - x - gap), fonts.body),
                           fonts.body, color);
        };
        cell(name_x, time_x, save.name);
        cell(time_x, row_w, game_time_string(save.game_time));
    });

    if (ctx.draw) {
        draw_preview(ctx, pane, has_selection() ? &g_rows[static_cast<std::size_t>(sel_)] : nullptr, fonts.caption);
        // A picked save of the other format keeps its file: one of the name is written beside it.
        const std::string hint = g_save
            ? std::format("Pick a save to overwrite, or type a new name. Saves as {}.",
                          asg_game_saves_asg() ? ".asg" : ".svl")
            : "Double-click or press Enter to load.";
        rf::gr::set_color(skin::ink_hint);
        rf::gr::string(body.x, hint_y, ui_fit_end(hint, body.w, fonts.caption).c_str(), fonts.caption);
    }
    build_footer(ctx, footer, fonts);
}

ColumnMenu g_menu;
SavedGamesPanel g_panel;
// Enter, Esc or Tab in the name box, run once the frame's hit-test pass has applied the keys typed before it.
std::optional<int> g_box_key;

// Stock's box has the typing back once a popup is gone, the keyboard focus with it.
void focus_name_box()
{
    g_menu.ui().set_focus(save_name_id, false);
    g_menu.ui().focus_text(save_name_id);
}

void save_message(const char* text)
{
    g_menu.popup().open(menu_stock_label(rf::strings::save_game), text,
                        {{menu_stock_label(rf::strings::ok), rf::KEY_O}}, 0, 0,
                        [](int, const std::string&) { focus_name_box(); }, MenuPopupStyle::Neutral);
}

void write_save(const std::string& path)
{
    rf::sr::save_game(path.c_str(), rf::local_player);
    // Stock empties its box whether or not the save worked.
    g_save_name.clear();
    rebuild_list();
    g_panel.clamp_selection();
    // A failed save has put up stock's error popup, which stock's message gives way to. The screen is not built
    // under it, so the box keeps the focus until it is gone.
    if (rf::ui::popup_is_active()) {
        focus_name_box();
        return;
    }
    save_message(rf::strings::game_saved);
}

void save_game()
{
    if (g_save_name.empty()) {
        save_message(rf::strings::no_filename);
        return;
    }
    if (!rf::sr::can_save_now()) {
        save_message(rf::strings::cannot_save_now);
        return;
    }
    // Stock's file_add_ext: the name's own extension replaced.
    std::string file = g_save_name;
    if (const std::size_t dot = file.rfind('.'); dot != std::string::npos) {
        file.resize(dot);
    }
    // The extension the save hook writes; the other format's file of the name is another save.
    const std::string path = std::string{rf::sr::savegame_path} + file + (asg_game_saves_asg() ? ".asg" : ".svl");
    // A file that cannot be checked is asked about, as one that is there.
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) && !ec) {
        write_save(path);
        return;
    }
    // NO on Enter, as stock's default.
    g_menu.popup().open(menu_stock_label(rf::strings::save_game), rf::strings::overwrite_save,
                        {
                            {menu_stock_label(rf::strings::no), rf::KEY_N},
                            {menu_stock_label(rf::strings::yes), rf::KEY_Y},
                        },
                        0, 0, [path](int button, const std::string&) {
                            if (button == 1) {
                                write_save(path);
                            }
                            else {
                                focus_name_box();
                            }
                        });
}

void load_game()
{
    const int sel = g_panel.selected();
    if (sel < 0 || sel >= row_count()) {
        return;
    }
    const SaveRow& row = g_rows[static_cast<std::size_t>(sel)];
    if (row.header) {
        const std::filesystem::path path = std::filesystem::path{rf::sr::savegame_path} / row.filename;
        if (asg_load_game(path)) {
            asg_set_last_save(path);
        }
    }
    else if (row.stock_index < rf::ui::load_game_list.size()) {
        const auto load = [index = row.stock_index] {
            rf::ui::load_game_sel_idx = index;
            rf::ui::load_game_load_click();
        };
        if (!asg_svl_load_converts()) {
            load();
            return;
        }
        g_menu.popup().open(asg_svl_load_title, asg_svl_load_text, {{"CONTINUE", rf::KEY_C}, {"CANCEL"}}, 1, 0,
                            [load](int button, const std::string&) {
                                if (button == 0) {
                                    load();
                                }
                            });
    }
}

void delete_message(std::string text)
{
    g_menu.popup().open(delete_title, std::move(text), {{menu_stock_label(rf::strings::ok), rf::KEY_O}}, 0, 0,
                        nullptr, MenuPopupStyle::Neutral);
}

// `name` is a row of the list: a file found in the savegame folder and read as a save. A quick
// save or an autosave is an ordinary one, and stock checks a save is still there before it offers to load it.
void delete_save(const std::string& name)
{
    // Never a path out of the folder, whatever the list holds.
    const bool bare = (string_iends_with(name, ".svl") || string_iends_with(name, ".asg"))
        && name.find_first_of("\\/:") == std::string::npos;
    const std::filesystem::path path = std::string{rf::sr::savegame_path} + name;
    // MSVC's remove clears the read-only attribute first; a save marked read-only stays, as with DeleteFile.
    std::error_code status_ec;
    const bool read_only = bare && (std::filesystem::status(path, status_ec).permissions() &
                                    std::filesystem::perms::owner_write) == std::filesystem::perms::none;
    std::error_code ec;
    const bool removed = bare && !read_only && std::filesystem::remove(path, ec);
    rebuild_list();
    g_panel.clamp_selection();
    // The focus may be on a DELETE that has just been disabled.
    if (g_save) {
        focus_name_box();
    }
    if (removed) {
        ui_play_toggle(false);
        return;
    }
    if (bare && !read_only && !ec) {
        delete_message(std::format("\"{}\" no longer exists.", name));
        return;
    }
    if (ec) {
        xlog::warn("[SaveLoad] Could not delete saved game {}: {}", name, ec.message());
    }
    delete_message(std::format("\"{}\" could not be deleted. It may be read-only or in use.", name));
}

void confirm_delete()
{
    const int sel = g_panel.selected();
    if (sel < 0 || sel >= row_count()) {
        return;
    }
    ui_play_click();
    std::string name = g_rows[static_cast<std::size_t>(sel)].filename;
    std::string text = std::format("Delete the saved game \"{}\"?", name);
    // NO on Enter, as for an overwrite.
    g_menu.popup().open(delete_title, std::move(text),
                        {
                            {menu_stock_label(rf::strings::no), rf::KEY_N},
                            {menu_stock_label(rf::strings::yes), rf::KEY_Y},
                        },
                        0, 0, [name = std::move(name)](int button, const std::string&) {
                            if (button == 1) {
                                delete_save(name);
                            }
                            else if (g_save) {
                                focus_name_box();
                            }
                        });
}

void act()
{
    ui_play_click();
    if (g_save) {
        save_game();
    }
    else {
        load_game();
    }
}

void run_action(SaveLoadAction action)
{
    switch (action) {
        case SaveLoadAction::Act:
            act();
            break;
        case SaveLoadAction::Delete:
            confirm_delete();
            break;
        case SaveLoadAction::Back:
            ui_play_click();
            if (!g_menu.closing()) {
                g_menu.request_close();
                rf::gameseq_pop_state();
            }
            break;
    }
}

void run_box_key(int key)
{
    const int code = key & rf::KEY_MASK;
    if (code == rf::KEY_ESC) {
        run_action(SaveLoadAction::Back);
        return;
    }
    if (code != rf::KEY_TAB) {
        run_action(SaveLoadAction::Act);
        return;
    }
    UiContext& ui = g_menu.ui();
    if (ui.focused_text_id() == save_name_id) {
        ui.release_text_focus(true);
        ui.set_focus(save_name_id, true);
        g_panel.tab(ui, key);
    }
}

void __cdecl key_callback(int key)
{
    // Stock's name box saves on Enter and goes back on Esc. Esc has let go of the box already, so the rest of the poll
    // would reach the screen's keys as it goes back.
    if (g_box_key && (*g_box_key & rf::KEY_MASK) == rf::KEY_ESC) {
        return;
    }
    const int code = key & rf::KEY_MASK;
    UiContext& ui = g_menu.ui();
    if (!g_menu.popup().active() && ui.focused_text_id() == save_name_id) {
        // After Enter or Tab, a later Enter, Tab or Esc and arrows in the same poll are dropped: they would change the
        // name or the focus before the first one runs.
        const bool ends_edit =
            code == rf::KEY_ESC || code == rf::KEY_TAB || code == rf::KEY_ENTER || code == rf::KEY_PADENTER;
        const bool arrow = key == rf::KEY_UP || key == rf::KEY_DOWN;
        if (g_box_key && (ends_edit || arrow)) {
            return;
        }
        // Stock's arrows step the list under the box, the name following.
        if (arrow) {
            g_panel.step(ui, key);
            return;
        }
        if (code == rf::KEY_ESC) {
            // At once: the box's own Esc would click as well as Back.
            ui.release_text_focus(true);
            g_box_key = key;
            return;
        }
        // The box takes no Tab; it leaves the box once the frame has applied the typing.
        if (code == rf::KEY_TAB) {
            g_box_key = key;
            return;
        }
        // Enter joins the box's queue, so the characters typed before it reach the name before the save reads it.
        if (code == rf::KEY_ENTER || code == rf::KEY_PADENTER) {
            g_box_key = key;
        }
    }
    g_menu.key(key);
}

} // namespace

void save_load_menu_enter(rf::GameState state)
{
    g_save = state == rf::GS_SAVE_GAME_MENU;
    rebuild_list();
    g_panel.reset();
    g_box_key.reset();
    const int act_id = static_cast<int>(SaveLoadAction::Act);
    const int back_id = static_cast<int>(SaveLoadAction::Back);
    g_menu.enter(menu_stock_label(g_save ? rf::strings::save_game : rf::strings::load_game),
                 {ColumnButton{act_id, menu_stock_label(g_save ? rf::strings::save : rf::strings::load),
                               g_save ? rf::KEY_S : rf::KEY_L}},
                 ColumnButton{back_id, menu_stock_label(rf::strings::save_load_back), rf::KEY_B}, back_id);
    g_menu.add_panel(&g_panel, act_id);
    g_menu.seat_panel(&g_panel);
    // Typing goes to the name box at once, as stock's has the focus.
    if (g_save) {
        g_menu.ui().focus_text(save_name_id);
    }
}

void save_load_menu_do_frame(bool paused)
{
    g_menu.step();
    // paused = 1 means a stock popup owns input this frame; it renders after us and on top.
    if (!paused) {
        const std::optional<int> action = g_menu.input(key_callback);
        const std::optional<int> box_key = std::exchange(g_box_key, std::nullopt);
        // Not under a popup the panel's commit opened, which another would open over.
        if (!g_menu.popup().active()) {
            if (action) {
                run_action(static_cast<SaveLoadAction>(*action));
            }
            else if (box_key) {
                run_box_key(*box_key);
            }
        }
        // Stock's box never lets go of the typing, so letters never reach the column's hotkeys: it has it back after a
        // click elsewhere, a delete or the console, unless the keyboard focus shows on another widget.
        UiContext& ui = g_menu.ui();
        if (g_save && !g_menu.closing() && !g_menu.popup().active() && !rf::console::console_is_visible()
            && ui.focused_text_id() == 0 && !ui.focus_visible()) {
            focus_name_box();
        }
    }
    g_menu.draw();
}

bool save_load_menu_is_closed()
{
    // A change the screen did not start, a load among them, does not wait for the slide.
    if (rf::gameseq_get_pending_state() != rf::GS_NONE || !g_menu.closing()) {
        return true;
    }
    return g_menu.is_closed();
}
