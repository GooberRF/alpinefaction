#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <common/utils/int-utils.h>
#include <common/utils/string-utils.h>
#include "create_game_internal.h"
#include "form_view.h"
#include "menu_shell.h"
#include "settings_skin.h"
#include "ui_kit.h"
#include "../misc/waypoints.h"
#include "../multi/map_thumbnails.h"
#include "../multi/multi.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/input.h"
#include "../rf/localize.h"
#include "../rf/ui.h"

namespace
{

using Clock = std::chrono::steady_clock;

constexpr std::chrono::milliseconds paste_blink{120};
constexpr int paste_blinks = 3;

bool entry_rule_shown(const ListenServerRule& rule, int scope)
{
    return !rule.root && listen_server_rule_applies(rule, create_game_setup().game_type(scope));
}

bool bots_on()
{
    return !create_game_setup().bots().empty()
        || create_game_setup().own_mutator(ListenServerSetup::base_scope, "joinbots").value_or(false);
}

// Lowercase filenames of the rotation's levels.
std::set<std::string> rotation_filenames()
{
    std::set<std::string> names;
    for (int i = 0; i < create_game_setup().num_levels(); ++i) {
        names.insert(string_to_lower(create_game_setup().level_filename(i)));
    }
    return names;
}

std::string type_list(const std::vector<rf::NetGameType>& types)
{
    std::string out;
    for (const rf::NetGameType type : types) {
        out += (out.empty() ? "" : ", ") + std::string{multi_game_type_name_short(type)};
    }
    return out;
}

// The map list's filters, each a game type whose level names it matches, or rf::NG_TYPE_UNK for every map.
constexpr std::array map_filters{rf::NG_TYPE_UNK, rf::NG_TYPE_DM, rf::NG_TYPE_CTF, rf::NG_TYPE_KOTH, rf::NG_TYPE_DC,
                                 rf::NG_TYPE_ESC, rf::NG_TYPE_REV, rf::NG_TYPE_PAY, rf::NG_TYPE_RUN};

int map_filter_for(rf::NetGameType game_type)
{
    if (game_type == rf::NG_TYPE_TEAMDM) {
        return rf::NG_TYPE_DM;
    }
    if (game_type == rf::NG_TYPE_SAL) {
        return rf::NG_TYPE_CTF;
    }
    return std::ranges::find(map_filters, game_type) != map_filters.end() ? game_type : rf::NG_TYPE_UNK;
}

// Stock levels lead an unsearched list in this order.
constexpr auto stock_levels = std::to_array<std::string_view>({
    "dm01", "dm02", "dm03", "dm04", "dm05", "dm06", "dm07", "dm08", "dm09", "dm10",
    "dm11", "dm12", "dm13", "dm14", "dm15", "dm16", "dm17", "dm18", "dm19", "dm20",
    "ctf01", "ctf02", "ctf03", "ctf04", "ctf05", "ctf06", "ctf07",
});

int stock_rank(std::string_view filename)
{
    const std::string name = level_filename_without_ext(filename);
    return static_cast<int>(std::ranges::find_if(stock_levels, [&](std::string_view level) {
        return string_iequals(level, name);
    }) - stock_levels.begin());
}

} // namespace

void CreateGameMapsPanel::reset()
{
    open_ = false;
    search_.clear();
    show_ = rf::NG_TYPE_UNK;
    list_scroll_ = 0.0f;
    list_scrolled_to_ = 0;
    selected_ = -1;
    reveal_ = false;
    rotation_scroll_ = 0.0f;
    rotation_scrolled_to_ = 0;
    hover_.clear();
    hover_pass_.clear();
    has_waypoints_.clear();
    edit_ = -1;
    clipboard_.reset();
    pasted_ = -1;
    filter();
}

void CreateGameMapsPanel::close()
{
    open_ = false;
    edit_ = -1;
    hover_.clear();
}

void CreateGameMapsPanel::follow_game_type(rf::NetGameType game_type)
{
    // Unchanged, the list keeps its scroll.
    if (const int show = map_filter_for(game_type); show != show_) {
        show_ = show;
        filter();
    }
}

void CreateGameMapsPanel::filter()
{
    const std::vector<CreateGameMap>& maps = create_game().maps;
    shown_.clear();
    list_scroll_ = 0.0f;
    for (int i = 0; i < static_cast<int>(maps.size()); ++i) {
        const CreateGameMap& map = maps[i];
        if (show_ < rf::NG_TYPE_UNK
            && !multi_level_name_matches_game_type(map.filename, static_cast<rf::NetGameType>(show_))) {
            continue;
        }
        const bool found = search_.empty() || all_words_match(search_, [&](std::string_view word) {
            return string_icontains(map.name, word) || string_icontains(map.filename, word);
        });
        if (found) {
            shown_.push_back(i);
        }
    }
    if (search_.empty()) {
        std::ranges::stable_sort(shown_, {}, [&](int i) { return stock_rank(maps[i].filename); });
    }
}

rf::NetGameType CreateGameMapsPanel::default_type_for(const std::string& filename) const
{
    const std::vector<rf::NetGameType> types = listen_server_game_types_for_level(filename);
    const auto lists = [&](int type) { return std::find(types.begin(), types.end(), type) != types.end(); };
    // The Game Type where it can run the map, so a filter browsed under GG or TDM does not override it.
    const rf::NetGameType game_type = create_game().game_type;
    if (lists(game_type) || types.empty()) {
        return game_type;
    }
    if (show_ < rf::NG_TYPE_UNK && lists(show_)) {
        return static_cast<rf::NetGameType>(show_);
    }
    return types.front();
}

void CreateGameMapsPanel::add(const std::string& filename)
{
    create_game_setup().add_level(filename, default_type_for(filename));
    selected_ = create_game_setup().num_levels() - 1;
    reveal_ = true;
}

void CreateGameMapsPanel::move(int to)
{
    const int count = create_game_setup().num_levels();
    if (selected_ < 0 || selected_ >= count || to < 0 || to >= count || to == selected_) {
        return;
    }
    create_game_setup().move_level(selected_, to);
    selected_ = to;
    pasted_ = -1;
    reveal_ = true;
}

void CreateGameMapsPanel::remove(int index)
{
    if (index < 0 || index >= create_game_setup().num_levels()) {
        return;
    }
    create_game_setup().remove_level(index);
    pasted_ = -1;
    // The selection stays on its entry, or takes the next one.
    if (index < selected_) {
        --selected_;
    }
    selected_ = std::min(selected_, create_game_setup().num_levels() - 1);
}

void CreateGameMapsPanel::copy_settings(int entry)
{
    clipboard_ = create_game_setup().level_settings(entry);
    ui_play_click();
}

void CreateGameMapsPanel::paste_settings(int entry)
{
    // Replaces, never merges: the entry ends as the copied one was.
    create_game_setup().set_level_settings(entry, *clipboard_);
    pasted_ = entry;
    pasted_at_ = Clock::now();
    ui_play_click();
}

void CreateGameMapsPanel::open_edit(int entry)
{
    selected_ = entry;
    edit_ = entry;
    edit_scroll_ = 0.0f;
    edit_scrolled_to_ = 0;
    hover_.clear();
    hover_pass_.clear();
}

bool CreateGameMapsPanel::has_waypoints(const std::string& filename) const
{
    const auto [it, added] = has_waypoints_.try_emplace(string_to_lower(filename), false);
    if (added) {
        it->second = waypoints_level_has_awp(filename);
    }
    return it->second;
}

bool CreateGameMapsPanel::key(UiContext& ctx, int key)
{
    if (ctx.focused_text_id() != 0 || selected_ < 0 || selected_ >= create_game_setup().num_levels()) {
        return false;
    }
    if (key == rf::KEY_DELETE) {
        remove(selected_);
        ui_play_click();
        return true;
    }
    if (key == (rf::KEY_C | rf::KEY_CTRLED)) {
        copy_settings(selected_);
        return true;
    }
    if (key == (rf::KEY_V | rf::KEY_CTRLED) && clipboard_) {
        paste_settings(selected_);
        return true;
    }
    return false;
}

void CreateGameMapsPanel::hints(const UiContext& ctx, std::vector<MenuHint>& hints) const
{
    // As key().
    if (ctx.focused_text_id() != 0 || selected_ < 0 || selected_ >= create_game_setup().num_levels()) {
        return;
    }
    hints.push_back({"DEL", "Remove"});
    hints.push_back({"CTRL+C", "Copy settings"});
    if (clipboard_) {
        hints.push_back({"CTRL+V", "Paste settings"});
    }
}

void CreateGameMapsPanel::commit()
{
    create_game_flush_edits();
}

void CreateGameMapsPanel::build_list(UiContext& ctx, const UiRect& well, int font)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const std::vector<CreateGameMap>& maps = create_game().maps;
    const std::set<std::string> in_rotation = rotation_filenames();
    const char* empty = nullptr;
    if (shown_.empty()) {
        empty = maps.empty() ? "No multiplayer maps installed" : "No maps match";
    }
    std::optional<std::string> added;
    const SkinScrollList list{
        .count = static_cast<int>(shown_.size()),
        .row_h = skin_list_row_h(ctx, font),
        .id_name = "cg_maps",
        .row_id_name = "cg_map",
        .empty_text = empty,
        .font = font,
    };
    skin_scroll_list(ctx, well, list, list_scroll_, list_scrolled_to_, [&](const SkinScrollListRow& row) {
        const CreateGameMap& map = maps[shown_[row.index]];
        if (!row.in_view) {
            if (row.offscreen_activated) {
                added = map.filename;
            }
            return;
        }
        const SkinListRow r = skin_list_row(ctx, row.rect, false, row.id);
        if (!ctx.draw) {
            if (ui_hover(ctx, row.rect, row.id)) {
                hover_pass_ = map.filename;
                hover_pass_rect_ = row.rect;
                hover_pass_well_ = well;
            }
            if (r.activated || r.clicked) {
                added = map.filename;
            }
            return;
        }
        const UiRect& d = r.draw;
        skin_led(ctx, d.x + s(14.0f), d.y + d.h / 2, 7.0f, 22.0f, in_rotation.contains(string_to_lower(map.filename)));
        const int text_x = d.x + s(32.0f);
        skin_draw_text(text_x, d.y + (d.h - rf::gr::get_font_height(font)) / 2,
                       ui_fit_end(map.name, d.x + d.w - s(8.0f) - text_x, font), font,
                       r.lit ? skin::txt_hi : skin::txt);
    });
    if (added) {
        // A map can be in the rotation more than once, as in a server config.
        add(*added);
        ui_play_click();
    }
}

void CreateGameMapsPanel::build_rotation(UiContext& ctx, const UiRect& well, int font)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const int count = create_game_setup().num_levels();
    const int row_h = skin_list_row_h(ctx, font);
    const bool show_waypoints = bots_on();
    const bool blinking = pasted_ >= 0 && Clock::now() - pasted_at_ < 2 * paste_blinks * paste_blink;
    const bool blink_on = blinking && ((Clock::now() - pasted_at_) / paste_blink) % 2 == 0;
    const UiId drag_id = ui_id("cg_rotation_drag");
    std::optional<int> picked;
    std::optional<int> removed;
    std::optional<int> edited;
    std::optional<int> start_y;
    std::optional<UiRect> sel_rect;
    const SkinScrollList list{
        .count = count,
        .row_h = row_h,
        .id_name = "cg_rotation",
        .row_id_name = "cg_rotation_entry",
        .empty_text = count == 0 ? "Click a map to add it" : nullptr,
        .font = font,
    };
    skin_scroll_list(ctx, well, list, rotation_scroll_, rotation_scrolled_to_, [&](const SkinScrollListRow& row) {
        const int i = row.index;
        if (!start_y) {
            start_y = row.rect.y;
        }
        if (i == selected_) {
            sel_rect = row.rect;
        }
        const CreateGameEntry entry = create_game_entry(ctx, row, s(170.0f));
        const UiId type_id = ui_id("cg_rotation_type", i);
        const UiId edit_id = ui_id("cg_rotation_edit", i);
        if (!row.in_view) {
            if (row.offscreen_activated) {
                picked = i;
            }
            ctx.register_focusable(edit_id, UiFocusKind::Activate);
            ctx.register_focusable(type_id, UiFocusKind::Adjustable);
            return;
        }
        const SkinListRow r = skin_list_row(ctx, row.rect, i == selected_, row.id);
        const std::string& filename = create_game_setup().level_filename(i);
        const rf::NetGameType type = create_game_setup().game_type(i);
        const std::string type_name{multi_game_type_name_short(type)};
        if (const int step = skin_cycler(ctx, entry.cycler, type_name.c_str(), font, true, type_id, entry.on_cycler);
            step != 0 && !ctx.draw) {
            std::vector<rf::NetGameType> types = listen_server_game_types_for_level(filename);
            if (types.empty()) {
                types.push_back(type);
            }
            const auto at = std::find(types.begin(), types.end(), type);
            const int n = static_cast<int>(types.size());
            const int current = at != types.end() ? static_cast<int>(at - types.begin()) : 0;
            create_game_setup().set_level_game_type(i, types[wrap_step(current, step, n)]);
            ui_play_click();
        }
        if (create_game_edit_button(ctx, entry.edit, create_game_setup().level_has_own_settings(i),
                                    i == pasted_ && blink_on, edit_id)) {
            edited = i;
        }
        if (!ctx.draw) {
            if (entry.on_row()) {
                hover_pass_ = filename;
                hover_pass_rect_ = row.rect;
                hover_pass_well_ = well;
            }
            switch (create_game_entry_click(ctx, r, entry)) {
                case CreateGameEntryClick::Pick:
                    picked = i;
                    break;
                case CreateGameEntryClick::Remove:
                    removed = i;
                    break;
                case CreateGameEntryClick::Edit:
                    edited = i;
                    break;
                case CreateGameEntryClick::None:
                    break;
            }
            return;
        }
        const UiRect& d = r.draw;
        const int ty = d.y + (d.h - rf::gr::get_font_height(font)) / 2;
        const int text_x = create_game_draw_entry_index(ctx, d, i, font);
        int text_r = ui_draw_rect(ctx, entry.edit).x - s(12.0f);
        if (show_waypoints && has_waypoints(filename)) {
            const int badge_font = menu_font(false, 17.0f, ctx.scale());
            const int badge_w = skin_badge_width(ctx, "!", badge_font, false);
            const int badge_h = s(skin::badge_h);
            const UiRect badge{entry.edit.x - s(12.0f) - badge_w, row.rect.y + (row.rect.h - badge_h) / 2, badge_w,
                               badge_h};
            skin_badge(ctx, badge, "!", skin::good, badge_font, nullptr, false);
            text_r -= badge_w + s(12.0f);
        }
        // Left out where not even "..." fits, rather than run into the badge.
        const std::string name = ui_fit_end(create_game_map_name(filename), text_r - text_x, font);
        if (rf::gr::get_string_size(name, font).first <= text_r - text_x) {
            skin_draw_text(text_x, ty, name, font, r.lit ? skin::txt_hi : skin::txt);
        }
    });
    if (ctx.draw) {
        return;
    }
    if (removed) {
        remove(*removed);
        ui_play_click();
    }
    else if (edited) {
        open_edit(*edited);
        ui_play_click();
        return;
    }
    else if (picked) {
        // The press picks the entry up; it drops where the button is let go.
        selected_ = *picked;
        if (ctx.click) {
            ctx.capture(drag_id);
        }
        ui_play_click();
    }
    if (ctx.captured_id() == drag_id && start_y && selected_ >= 0 && create_game_setup().num_levels() > 0) {
        move(std::clamp((ctx.my - *start_y) / row_h, 0, create_game_setup().num_levels() - 1));
        reveal_ = false;
    }
    if (std::exchange(reveal_, false) && sel_rect) {
        create_game_reveal(ctx, well, *sel_rect, rotation_scroll_);
    }
}

void CreateGameMapsPanel::draw_card(const UiContext& ctx, const std::string& filename, const UiRect& row,
                                    const UiRect& well) const
{
    const auto s = [&](float v) { return ctx.scaled(v); };
    const int font = menu_font(false, 18.0f, ctx.scale());
    const int bold = menu_font(true, 20.0f, ctx.scale());
    const int line_h = rf::gr::get_font_height(font) + s(4.0f);
    // Within the row's list, so the other list stays in view, and between the path strip and the hint bar.
    const int edge = ctx.scaled(12.0f);
    const int strip_h = skin_px(ctx, menu_strip_h);
    const UiRect bounds{well.x - edge, strip_h, well.w + 2 * edge, rf::gr::clip_height() - 2 * strip_h};
    const MenuCard card = menu_card(ctx, bounds);
    const int inner = card.inner;
    const int cap_h = rf::gr::get_font_height(font) + s(8.0f);
    // A level known to have no image keeps only the caption band.
    const int thumb_h = map_thumbnail_missing(filename) ? cap_h : inner * 9 / 16;

    struct Line
    {
        std::string text;
        rf::Color color;
    };
    std::vector<Line> lines;
    if (const MapLevelInfo* info = map_level_info_get(filename); info && !info->author.empty()) {
        lines.push_back({"by " + info->author, skin::txt});
    }
    const std::string types = "Game types: " + type_list(listen_server_game_types_for_level(filename));
    for (std::string& text : ui_wrap_text(types, inner, font)) {
        lines.push_back({std::move(text), skin::txt_hint});
    }
    if (bots_on() && has_waypoints(filename)) {
        for (std::string& text : ui_wrap_text("Bot waypoints available", inner, font)) {
            lines.push_back({std::move(text), skin::good});
        }
    }
    const int h = card.pad + thumb_h + s(8.0f) + rf::gr::get_font_height(bold) + s(4.0f)
        + static_cast<int>(lines.size()) * line_h + card.pad;
    const UiRect r = menu_card_place(ctx, card, h, row, well.x, bounds);
    menu_draw_card(ctx, r);
    const int cx = r.x + card.pad;
    int cy = r.y + card.pad;
    const std::string level = level_filename_without_ext(filename);
    const std::optional<rf::NetGameType> prefix_type = multi_game_type_for_level_prefix(filename);
    const MenuLevelCaption caption{
        .level = level,
        .type = prefix_type ? multi_game_type_name_short(*prefix_type) : std::string_view{},
        .font = font,
        .cap_h = cap_h,
        .pad = s(8.0f),
        .level_color = skin::txt_hi,
        .type_color = skin::txt,
    };
    menu_draw_level_preview(filename, UiRect{cx, cy, inner, thumb_h}, caption);
    cy += thumb_h + s(8.0f);
    skin_draw_text(cx, cy, ui_fit_end(create_game_map_name(filename), inner, bold), bold, skin::txt_hi);
    cy += rf::gr::get_font_height(bold) + s(4.0f);
    for (const Line& line : lines) {
        skin_draw_text(cx, cy, ui_fit_end(line.text, inner, font), font, line.color);
        cy += line_h;
    }
}

void CreateGameMapsPanel::build(UiContext& ctx, const MenuPanel& panel, const MenuFonts& fonts)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const int font = fonts.body;
    if (!ctx.draw) {
        hover_pass_.clear();
    }
    else {
        menu_draw_panel_heading(ctx, panel, "MAPS", fonts.heading);
    }
    {
        const char* text = "Random order";
        const bool random = create_game_setup().random_rotation();
        const SkinCyclerRects rects = skin_header_cycler_rects(ctx, panel.header, text, 90.0f, font);
        if (skin_labeled_cycler(ctx, rects.label, rects.toggle, text, random ? "On" : "Off", font, true,
                                ui_id("cg_random_rotation"), skin::ink_title, skin::ink_status)
            != 0) {
            create_game_setup().set_random_rotation(!random);
            ui_play_toggle(!random);
        }
    }

    const int count = create_game_setup().num_levels();
    const CreateGamePanes panes = create_game_panes(
        ctx, panel, "ROTATION", count == 1 ? std::string{"1 map"} : std::format("{} maps", count), fonts);
    const UiRect& bar = panes.bar;
    const int show_w = std::min(s(250.0f), bar.w / 2);
    const UiRect search{bar.x, bar.y, bar.w - show_w - s(12.0f), bar.h};
    if (skin_search_box(ctx, search, ui_id("cg_map_search"), search_, font, "Search maps...")) {
        filter();
    }
    const std::string show_text = multi_game_type_maps_label(
        show_ < rf::NG_TYPE_UNK ? std::optional{static_cast<rf::NetGameType>(show_)} : std::nullopt);
    const UiRect show_rect{bar.x + bar.w - show_w, bar.y, show_w, bar.h};
    if (const int step =
            skin_cycler(ctx, show_rect, show_text.c_str(), font, true, ui_id("cg_map_show"), ui_hover(ctx, show_rect));
        step != 0 && !ctx.draw) {
        const auto current = std::ranges::find(map_filters, show_) - map_filters.begin();
        show_ = map_filters[wrap_step(static_cast<int>(current), step, static_cast<int>(map_filters.size()))];
        ui_play_click();
        filter();
    }

    build_list(ctx, panes.left, font);
    build_rotation(ctx, panes.right, font);

    if (skin_button(ctx, panes.left_footer, rf::strings::add_all, font, SkinButton::Secondary, !shown_.empty(),
                    ui_id("cg_add_all"))) {
        const std::set<std::string> in_rotation = rotation_filenames();
        for (const int map : shown_) {
            const std::string& filename = create_game().maps[map].filename;
            if (!in_rotation.contains(string_to_lower(filename))) {
                add(filename);
            }
        }
        ui_play_click();
    }
    switch (create_game_list_buttons(ctx, panes.right_footer, selected_, create_game_setup().num_levels(), true, font,
                                     "cg_rotation_op")) {
        case CreateGameListOp::Up:
            move(selected_ - 1);
            break;
        case CreateGameListOp::Down:
            move(selected_ + 1);
            break;
        case CreateGameListOp::Remove:
            remove(selected_);
            break;
        case CreateGameListOp::Clear:
            while (create_game_setup().num_levels() > 0) {
                create_game_setup().remove_level(0);
            }
            selected_ = -1;
            pasted_ = -1;
            break;
        case CreateGameListOp::None:
            break;
    }

    // The card waits for the cursor to rest on one row, and stays away while a button is held.
    if (!ctx.draw) {
        if (ctx.lmb_down) {
            hover_pass_.clear();
        }
        hover_.track(hover_pass_);
        hover_rect_ = hover_pass_rect_;
        hover_well_ = hover_pass_well_;
    }
    else if (hover_.rested() && edit_ < 0 && !create_game_popup_active() && !rf::ui::popup_is_active()) {
        ctx.push_overlay([this, &ctx, filename = hover_.key(), row = hover_rect_, well = hover_well_] {
            draw_card(ctx, filename, row, well);
        });
    }
}

// ─── An entry's own settings ───

bool CreateGameMapsPanel::modal_key(UiContext& ctx, int key)
{
    if ((key & rf::KEY_MASK) != rf::KEY_ESC) {
        return false;
    }
    ctx.release_text_focus(true);
    create_game_flush_edits();
    edit_ = -1;
    ui_play_click();
    return true;
}

void CreateGameMapsPanel::build_modal(UiContext& ctx)
{
    if (edit_ < 0 || edit_ >= create_game_setup().num_levels()) {
        edit_ = -1;
        return;
    }
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const float scale = menu_screen_scale();
    const FormFonts fonts{menu_font(false, 20.0f, scale), menu_font(true, 20.0f, scale),
                          menu_font(false, 17.0f, scale)};
    const int scope = edit_;
    const std::string& filename = create_game_setup().level_filename(scope);

    UiRect dialog;
    const UiRect area = create_game_dialog(ctx, 1000, 780, "Map Settings", dialog);
    const std::string note = std::format("{} ({}), {}. The rows it does not set follow OPTIONS and MUTATORS.",
                                         create_game_map_name(filename), filename,
                                         multi_game_type_name(create_game_setup().game_type(scope)));
    const int note_h = create_game_dialog_note(ctx, area, note, fonts.body);
    const int btn_h = s(44.0f);
    const int btn_y = dialog.y + dialog.h - s(24.0f) - btn_h;
    const int field_y = area.y + note_h + s(8.0f);
    const UiRect field{area.x, field_y, area.w, btn_y - s(14.0f) - field_y};
    if (ctx.draw) {
        skin_steel_field(ctx, field.x, field.y, field.w, field.h);
    }

    const UiRect view{field.x + s(8.0f), field.y + s(8.0f), field.w - s(16.0f), field.h - s(16.0f)};
    const int row_h = skin_px(ctx, skin::row_h);
    const int sub_h = ctx.scaled(form_sub_header_h);
    const int content_h =
        create_game_rules_height(ctx, scope, entry_rule_shown) + sub_h + create_game_mutators_height(ctx, scope);
    FormScroll scroll = form_scroll_begin(ctx, view, content_h, edit_scroll_, row_h * 3, ui_id("cg_entry_rows"));
    int y = scroll.region.start_y();
    create_game_rule_rows(ctx, scroll, view.x, y, scope, entry_rule_shown, fonts);
    if (const auto rect = form_scroll_row(ctx, scroll, view.x, y, sub_h, 0, UiFocusKind::Activate)) {
        form_draw_sub_header(ctx, *rect, "Mutators", fonts.body_bold);
    }
    create_game_mutator_rows(ctx, scroll, view.x, y, scope, fonts);
    form_scroll_end(ctx, scroll, edit_scrolled_to_);

    const int btn_w = s(200.0f);
    const int btn_gap = s(16.0f);
    const UiRect copy{dialog.x + s(24.0f), btn_y, btn_w, btn_h};
    const UiRect paste{copy.x + btn_w + btn_gap, btn_y, btn_w, btn_h};
    const UiRect close_rect{dialog.x + dialog.w - s(24.0f) - btn_w, btn_y, btn_w, btn_h};
    const UiRect reset_all{close_rect.x - btn_gap - btn_w, btn_y, btn_w, btn_h};
    // A typed value lands in its cell and only reaches the setup on a flush, which would undo a replacement.
    if (skin_button(ctx, copy, "COPY", fonts.body_bold, SkinButton::Secondary, true, ui_id("cg_entry_copy"))) {
        ctx.release_text_focus(true);
        create_game_flush_edits();
        copy_settings(scope);
    }
    if (skin_button(ctx, paste, "PASTE", fonts.body_bold, SkinButton::Secondary, clipboard_.has_value(),
                    ui_id("cg_entry_paste"))
        && clipboard_) {
        ctx.release_text_focus(true);
        create_game_flush_edits();
        paste_settings(scope);
    }
    if (skin_button(ctx, reset_all, "RESET ALL", fonts.body_bold, SkinButton::Secondary,
                    create_game_setup().level_has_own_settings(scope), ui_id("cg_entry_reset_all"))) {
        ctx.release_text_focus(true);
        create_game_flush_edits();
        create_game_setup().set_level_settings(scope, toml::table{});
        ui_play_click();
    }
    if (skin_button(ctx, close_rect, "CLOSE", fonts.body_bold, SkinButton::Secondary, true, ui_id("cg_entry_close"))) {
        ctx.release_text_focus(true);
        create_game_flush_edits();
        edit_ = -1;
        ui_play_click();
    }
}
