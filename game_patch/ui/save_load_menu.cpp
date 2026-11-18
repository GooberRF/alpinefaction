#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <common/utils/string-utils.h>
#include <xlog/xlog.h>
#include "save_load_menu.h"
#include "column_menu.h"
#include "form_view.h"
#include "menu_popup.h"
#include "menu_shell.h"
#include "settings_skin.h"
#include "ui_kit.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/input.h"
#include "../rf/localize.h"
#include "../rf/os/console.h"
#include "../rf/player/player.h"
#include "../rf/save_restore.h"
#include "../rf/sound/sound.h"
#include "../rf/ui.h"
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
std::string date_string(int write_time)
{
    const std::time_t t = write_time;
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

rf::VArray<rf::ui::SavegameListEntry>& saved_games()
{
    return g_save ? rf::ui::save_game_list : rf::ui::load_game_list;
}

void rebuild_list()
{
    saved_games().clear();
    if (g_save) {
        rf::ui::save_game_build_list();
    }
    else {
        rf::ui::load_game_build_list();
    }
}

void run_action(SaveLoadAction action);
void focus_name_box();

class SavedGamesPanel final : public ColumnPanel
{
public:
    void reset()
    {
        // Load starts on the newest save, as stock's list; Save on none until one is picked.
        sel_ = !g_save && saved_games().size() > 0 ? 0 : -1;
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
        const int count = saved_games().size();
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
        return sel_ >= 0 && sel_ < saved_games().size();
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
        const std::string_view name = entry_name(saved_games()[static_cast<std::size_t>(row)]);
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
    const int count = saved_games().size();
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
    if (saved_games().size() > 0) {
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
            ctx.set_focus(ui_id(row_id_name, back ? 0 : saved_games().size() - 1), true);
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
    if (const int count = saved_games().size(); count > 0) {
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
    const bool has_rows = saved_games().size() > 0;
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
    if (hit && hit->row < saved_games().size()) {
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

void SavedGamesPanel::build(UiContext& ctx, const MenuPanel& panel, const MenuFonts& fonts)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const rf::VArray<rf::ui::SavegameListEntry>& list = saved_games();
    const UiRect& body = panel.body;
    const UiRect footer = form_panel_areas(ctx, panel, false).footer;
    const int hint_y = footer.y - s(10.0f) - rf::gr::get_font_height(fonts.caption);
    const UiRect well{body.x, body.y, body.w, hint_y - s(10.0f) - body.y};
    const int row_h = skin_list_row_h(ctx, fonts.body);

    // The text columns, from where skin_scroll_list puts its rows in the well. The date and the game time are never
    // cut, as stock sizes its columns to hold them.
    const int row_x = well.x + s(4.0f);
    const int row_w = well.w - s(8.0f) - ui_scrollbar_width(ctx) - s(4.0f);
    const int name_x = s(24.0f);
    const int gap = s(12.0f);
    const auto column_w = [&](const char* widest_value, const char* header) {
        return std::max(rf::gr::get_string_size(widest_value, fonts.body).first,
                        rf::gr::get_string_size(trimmed(header), fonts.column).first)
             + gap;
    };
    const int time_x = std::min(row_w * 78 / 100, row_w - column_w("00:00:00", rf::strings::game_time_column));
    const int date_x =
        std::min(row_w * 50 / 100, time_x - column_w("00/00/00 00:00:00", rf::strings::date_time_column));

    if (ctx.draw) {
        const UiRect& header = panel.header;
        const int y = header.y + (header.h - rf::gr::get_font_height(fonts.column)) / 2;
        rf::gr::set_color(skin::ink_title);
        rf::gr::string(row_x + name_x, y, trimmed(rf::strings::name_column).c_str(), fonts.column);
        rf::gr::string(row_x + date_x, y, trimmed(rf::strings::date_time_column).c_str(), fonts.column);
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
        .count = list.size(),
        .row_h = row_h,
        .id_name = "saved_games",
        .row_id_name = row_id_name,
        .empty_text = list.size() == 0 ? "No saved games." : nullptr,
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
        const rf::ui::SavegameListEntry& entry = list[static_cast<std::size_t>(row.index)];
        const rf::Color color = r.lit ? skin::txt_hi : skin::txt;
        const int text_y = r.draw.y + (r.draw.h - rf::gr::get_font_height(fonts.body)) / 2;
        const auto cell = [&](int x, int right, std::string_view text) {
            skin_draw_text(r.draw.x + x, text_y, ui_fit_end(text, std::max(1, right - x - gap), fonts.body),
                           fonts.body, color);
        };
        cell(name_x, date_x, entry_name(entry));
        cell(date_x, time_x, date_string(entry.write_time));
        cell(time_x, row_w, game_time_string(entry.game_time));
    });

    if (ctx.draw) {
        const char* hint =
            g_save ? "Pick a save to overwrite, or type a new name." : "Double-click or press Enter to load.";
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
    const std::string path = std::string{rf::sr::savegame_path} + file + ".svl";
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
    if (sel < 0 || sel >= rf::ui::load_game_list.size()) {
        return;
    }
    rf::ui::load_game_sel_idx = sel;
    rf::ui::load_game_load_click();
}

void delete_message(std::string text)
{
    g_menu.popup().open(delete_title, std::move(text), {{menu_stock_label(rf::strings::ok), rf::KEY_O}}, 0, 0,
                        nullptr, MenuPopupStyle::Neutral);
}

// `name` is a row of the stock list: a file the builder found in the savegame folder and read as a save. A quick
// save or an autosave is an ordinary one, and stock checks a save is still there before it offers to load it.
void delete_save(const std::string& name)
{
    // Never a path out of the folder, whatever the list holds.
    const bool bare = string_iends_with(name, ".svl") && name.find_first_of("\\/:") == std::string::npos;
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
    if (sel < 0 || sel >= saved_games().size()) {
        return;
    }
    ui_play_click();
    std::string name{entry_name(saved_games()[static_cast<std::size_t>(sel)])};
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
