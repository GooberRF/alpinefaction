#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <common/utils/int-utils.h>
#include <common/utils/string-utils.h>
#include "vote_menu.h"
#include "column_menu.h"
#include "form_view.h"
#include "menu_shell.h"
#include "mutator_editors.h"
#include "settings_skin.h"
#include "ui_kit.h"
#include "../input/input.h"
#include "../misc/saved_votes.h"
#include "../misc/vote_form.h"
#include "../multi/alpine_packets.h"
#include "../multi/multi.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/input.h"
#include "../rf/localize.h"
#include "../rf/player/player.h"

namespace
{

enum class Host
{
    None,
    PauseMenu,
    Game,
};

constexpr int saved_action = 100;
constexpr int back_action = 101;
constexpr int status_action = 102;

// Description ids of the rows that are not mutators, whose ids are their index in the schema.
constexpr int desc_team_size = 1000;
constexpr int desc_gametype = 1001;
constexpr int desc_filter = 1002;
constexpr int desc_manual = 1003;
constexpr int desc_level_name = 1004;
constexpr int desc_rotation_action = 1005;
constexpr int desc_preserve = 1006;
constexpr int desc_current_mutators = 1007;
constexpr int desc_base_mutators = 1008;

// The tabs, SAVED and BACK: more than the large buttons hold.
static_assert(vote_form_max_tabs + 2 <= menu_panel_slots);
ColumnMenu g_menu{MenuButtonSize::Small};
Host g_host = Host::None;
// Over the game, the name of the key bound to the menu's control, which also closes it; empty when unbound.
std::string g_close_key;
// The bind g_close_key was named from: the bind contexts can change it while the menu is open.
std::optional<std::array<int16_t, 3>> g_close_bind;
// Over the game, a close the frame's hit-test pass asked for.
bool g_close_pending = false;
VoteFormMode g_mode = VoteFormMode::Loading;
std::vector<VoteTab> g_tabs;
UiId g_prev_text_focus = 0;
float g_form_scroll = 0.0f;
UiId g_form_scrolled_to = 0;
UiId g_level_scrolled_to = 0;
UiId g_kick_scrolled_to = 0;
UiId g_saved_scrolled_to = 0;

void reset_description()
{
    g_menu.ui().description() = UiDescriptionState{};
}

void request_close()
{
    if (g_host == Host::PauseMenu) {
        g_menu.request_close();
    }
    else {
        g_close_pending = true;
    }
}

void draw_lines(const UiRect& r, const std::vector<std::string>& lines, int font, int line_h, rf::Color color)
{
    ui_draw_line_block(r, lines, font, line_h, color.red, color.green, color.blue);
}

void draw_message(const UiContext& ctx, const UiRect& body, const std::vector<std::string>& lines,
                  const MenuFonts& fonts)
{
    if (!ctx.draw) {
        return;
    }
    const int line_h = rf::gr::get_font_height(fonts.body) + ctx.scaled(8.0f);
    int y = body.y + (body.h - static_cast<int>(lines.size()) * line_h) / 2;
    rf::gr::set_color(skin::ink_status);
    for (const std::string& line : lines) {
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, body.x + body.w / 2, y, line.c_str(), fonts.body);
        y += line_h;
    }
}

void draw_paragraph(const UiContext& ctx, const UiRect& body, const char* text, const MenuFonts& fonts)
{
    if (ctx.draw) {
        draw_lines(body, {text}, fonts.body, rf::gr::get_font_height(fonts.body) + ctx.scaled(6.0f), skin::ink_status);
    }
}

const char* row_description(int id, const VoteOptionsData* options, const char* fallback)
{
    switch (id) {
        case desc_team_size:
            return "Players on each team of the match.";
        case desc_gametype:
            return "The game type the vote runs under. It follows the picked level until you change it; the reset "
                   "icon puts it back.";
        case desc_filter:
            return options && options->gametype_prefix_restricted
                ? "This server only accepts levels made for the game type, so only those are listed."
                : "List only the levels made for the chosen game type.";
        case desc_manual:
            return "Vote for a level by its file name, for one the list does not show.";
        case desc_level_name:
            return "The file name of the level to vote for.";
        case desc_preserve:
            return "Keep the game type and mutators the session runs now.";
        case desc_current_mutators:
            return "Back to the mutators the session runs now. The list follows them again until you change it.";
        case desc_base_mutators:
            return "The mutators of the server's base rules.";
        default:
            break;
    }
    if (options && id >= 0 && id < static_cast<int>(options->mutators.size())) {
        return mutator_description(options->mutators[id].name);
    }
    return fallback;
}

void build_description(UiContext& ctx, const UiRect& strip, const VoteOptionsData* options, const char* fallback,
                       const MenuFonts& fonts)
{
    if (ctx.draw) {
        form_draw_description(ctx, strip, row_description(ctx.description().id, options, fallback), fonts.caption);
    }
}

struct FooterButton
{
    const char* label;
    bool enabled;
};

// Returns the button pressed, -1 for none.
int build_footer(UiContext& ctx, const UiRect& line, std::span<const FooterButton> buttons, const MenuFonts& fonts)
{
    int pressed = -1;
    int x = line.x + line.w;
    for (int i = static_cast<int>(buttons.size()) - 1; i >= 0; --i) {
        const FooterButton& button = buttons[i];
        const int w = skin_button_width(ctx, button.label, fonts.column, 150.0f);
        const UiRect r{x - w, line.y, w, line.h};
        if (skin_button(ctx, r, button.label, fonts.column, SkinButton::Secondary, button.enabled,
                        ui_id("vote_footer", i))
            && button.enabled) {
            pressed = i;
        }
        x = r.x - ctx.scaled(16.0f);
    }
    return pressed;
}

void call_and_close(const VoteOptionsData& options)
{
    vote_form_call(options);
    ui_play_click();
    request_close();
}

void begin_save(const VoteOptionsData& options)
{
    if (!vote_form_begin_save(options)) {
        return;
    }
    ui_play_click();
    // Anything the store keeps; the name is sanitized once it is typed.
    const UiCharFilter name_char = [](char c) { return c >= ' ' && c != '|'; };
    g_menu.popup().open_input("Save Vote", "Name this saved vote:", "", 31, name_char, "SAVE",
                              [](int button, const std::string& text) {
                                  if (button == 1 && vote_form_finish_save(text)) {
                                      ui_play_toggle(true);
                                  }
                                  else {
                                      vote_form_drop_save();
                                  }
                              });
}

void build_vote_footer(UiContext& ctx, const UiRect& line, const VoteOptionsData& options, AfVoteType type,
                       const MenuFonts& fonts)
{
    std::vector<FooterButton> buttons;
    const bool savable = saved_vote_type_is_savable(type);
    if (savable) {
        buttons.push_back({"SAVE", vote_form_can_save(options)});
    }
    buttons.push_back({"CANCEL VOTE", vote_form_can_cancel()});
    buttons.push_back({"CALL VOTE", vote_form_can_call(options)});
    const int pressed = build_footer(ctx, line, buttons, fonts);
    const int first = savable ? 1 : 0;
    if (savable && pressed == 0) {
        begin_save(options);
    }
    else if (pressed == first) {
        vote_form_cancel();
        ui_play_click();
    }
    else if (pressed == first + 1) {
        call_and_close(options);
    }
}

// A list row's text, clear of the selection bar's LED and of `right` pixels at the row's end.
void draw_row_text(const UiContext& ctx, const SkinListRow& row, const std::string& text, int right, rf::Color color,
                   int font)
{
    const int text_x = row.draw.x + ctx.scaled(24.0f);
    const int text_w = std::max(1, row.draw.x + row.draw.w - right - ctx.scaled(12.0f) - text_x);
    const SkinText fitted = skin_fit_text(ctx, text, font, text_w);
    rf::gr::set_color(color);
    const int text_y = row.draw.y + (row.draw.h - rf::gr::get_font_height(fitted.font)) / 2;
    rf::gr::string(text_x, text_y, fitted.text.c_str(), fitted.font);
}

// A list of names in a well; returns the row picked by click or Enter, else -1.
int build_name_list(UiContext& ctx, const UiRect& well, const std::vector<std::string>& names, int selected,
                    float& scroll, UiId& scrolled_to, const char* id_name, const char* empty_text,
                    const MenuFonts& fonts)
{
    int picked = -1;
    const SkinScrollList list{
        .count = static_cast<int>(names.size()),
        .row_h = skin_list_row_h(ctx, fonts.body),
        .id_name = id_name,
        .row_id_name = id_name,
        .empty_text = names.empty() ? empty_text : nullptr,
        .font = fonts.body,
    };
    skin_scroll_list(ctx, well, list, scroll, scrolled_to, [&](const SkinScrollListRow& row) {
        if (!row.in_view) {
            if (row.offscreen_activated) {
                picked = row.index;
            }
            return;
        }
        const SkinListRow r = skin_list_row(ctx, row.rect, row.index == selected, row.id);
        if (ctx.draw) {
            draw_row_text(ctx, r, names[row.index], 0, r.lit ? skin::txt_hi : skin::txt, fonts.body);
        }
        else if (r.clicked || r.activated) {
            picked = row.index;
        }
    });
    return picked;
}

// One-shot: centers `row` in a list `well`, which skin_scroll_list then clamps.
void center_row(float& scroll, int row, int row_h, const UiRect& well, const UiContext& ctx)
{
    const int view_h = well.h - ctx.scaled(8.0f);
    scroll = static_cast<float>(row * row_h - view_h / 2 + row_h / 2);
}

// ─── Level and Match ───

// Shows `desc` in the strip while the cursor is on `r` or the keyboard on `id`.
void describe(UiContext& ctx, const UiRect& r, UiId id, int desc)
{
    if (!ctx.draw && (ui_hover(ctx, r) || (ctx.focus_visible() && ctx.focus_id() == id))) {
        ctx.description().hover(desc);
    }
}

std::string level_filter_label(bool on, uint8_t gametype)
{
    if (on && gametype >= rf::NG_TYPE_UNK) {
        return "Game type";
    }
    return multi_game_type_maps_label(on ? std::optional{static_cast<rf::NetGameType>(gametype)} : std::nullopt);
}

// The search box, and the game type filter at the line's end.
void build_level_search(UiContext& ctx, const UiRect& line, const VoteOptionsData& options,
                        const std::vector<const VoteGametypeInfo*>& gametypes, uint8_t gametype,
                        const MenuFonts& fonts)
{
    VoteForm& form = vote_form();
    int widest = rf::gr::get_string_size(level_filter_label(false, gametype), fonts.body).first;
    for (const VoteGametypeInfo* info : gametypes) {
        widest = std::max(widest, rf::gr::get_string_size(level_filter_label(true, info->id), fonts.body).first);
    }
    const int filter_w = std::min(skin_cycler_width(ctx, widest), line.w / 2);
    const UiRect search{line.x, line.y, line.w - filter_w - ctx.scaled(12.0f), line.h};
    if (skin_search_box(ctx, search, ui_id("vote_level_search"), form.level_filter, fonts.body, "Search...")) {
        vote_form_level_filter_changed();
    }

    const UiId id = ui_id("vote_filter");
    const UiRect filter{line.x + line.w - filter_w, line.y, filter_w, line.h};
    const bool forced = options.gametype_prefix_restricted;
    const bool on = vote_form_gametype_filter_active(options);
    describe(ctx, filter, id, desc_filter);
    const std::string label = level_filter_label(on, gametype);
    if (skin_cycler(ctx, filter, label.c_str(), fonts.body, !forced, id, ui_hover(ctx, filter)) != 0) {
        vote_form_set_gametype_filter(!on);
        ui_play_click();
    }
}

// The level typed by name instead of picked from the list.
void build_manual_level(UiContext& ctx, const UiRect& box, const UiRect& field, const MenuFonts& fonts)
{
    VoteForm& form = vote_form();
    const char* label = "Enter level name manually";
    const UiId box_id = ui_id("vote_manual");
    const UiRect lamp{box.x, box.y, std::min(box.w, skin_lamp_toggle_width(ctx, label, fonts.body)), box.h};
    describe(ctx, lamp, box_id, desc_manual);
    if (skin_lamp_toggle(ctx, lamp, label, form.manual_level, fonts.body, box_id)) {
        form.manual_level = !form.manual_level;
    }

    const UiId field_id = ui_id("vote_level_name");
    describe(ctx, field, field_id, desc_level_name);
    skin_field(ctx, field, field_id, form.manual_level_name, 31, ui_char_filter_filename, "Level file name", fonts.body,
               form.manual_level);
}

void build_level_list(UiContext& ctx, const UiRect& area, const VoteOptionsData& options, bool is_match,
                      uint8_t gametype, const MenuFonts& fonts)
{
    VoteForm& form = vote_form();
    const VoteLevelList& list = vote_form_level_list(options, is_match, gametype, !ctx.draw);
    const int level_rows = static_cast<int>(list.items.size()) - list.first_level_row;

    // Named apart: changing the game type will not bring back a map the allow-list hid.
    const char* hint = list.gametype_hidden > 0 ? "Some maps hidden: not valid for this game type"
        : list.not_allowed_hidden > 0           ? "Some maps hidden: not on the server vote list"
                                                : nullptr;
    const int hint_h = hint ? rf::gr::get_font_height(fonts.caption) + ctx.scaled(12.0f) : 0;
    const UiRect well{area.x, area.y, area.w, area.h - hint_h};
    const int row_h = skin_list_row_h(ctx, fonts.body);
    if (!ctx.draw && std::exchange(form.level_scroll_to_selection, false) && list.sel_row >= 0) {
        center_row(form.level_scroll, list.sel_row, row_h, well, ctx);
    }

    // The typed filter first: the maps it hid come back with backspace, not with another game type.
    const char* empty_text = list.name_filter_hidden > 0 ? "No maps match the filter"
        : list.gametype_hidden > 0                       ? "No maps available for this game type"
                                                         : "No maps available for voting on this server";
    const SkinScrollList scroll_list{
        .count = static_cast<int>(list.items.size()),
        .row_h = row_h,
        .id_name = "vote_level_list",
        .row_id_name = "vote_level",
        .empty_text = !is_match && level_rows == 0 ? empty_text : nullptr,
        .font = fonts.body,
    };
    int picked = -1;
    skin_scroll_list(ctx, well, scroll_list, form.level_scroll, g_level_scrolled_to, [&](const SkinScrollListRow& row) {
        if (!row.in_view) {
            if (row.offscreen_activated) {
                picked = row.index;
            }
            return;
        }
        const SkinListRow r = skin_list_row(ctx, row.rect, row.index == list.sel_row, row.id);
        if (ctx.draw) {
            const bool pinned = is_match && row.index == 0;
            draw_row_text(ctx, r, list.items[row.index], 0,
                          r.lit ? skin::txt_hi : (pinned ? skin::txt_hint : skin::txt), fonts.body);
        }
        else if (r.clicked || r.activated) {
            picked = row.index;
        }
    });
    if (picked >= 0) {
        vote_form_pick_level_row(picked, is_match);
        ui_play_click();
    }

    if (ctx.draw && hint) {
        const SkinText fitted = skin_fit_text(ctx, hint, fonts.caption, area.w);
        rf::gr::set_color(skin::ink_hint);
        rf::gr::string(area.x, area.y + area.h - rf::gr::get_font_height(fitted.font), fitted.text.c_str(),
                       fitted.font);
    }
}

struct MutatorRows
{
    std::vector<std::vector<MutatorEditorOption>> options;
    std::vector<MutatorEditor> editors;
};

MutatorRows mutator_rows(const VoteOptionsData& options, uint8_t game_type)
{
    MutatorRows rows;
    rows.options.reserve(options.mutators.size());
    rows.editors.reserve(options.mutators.size());
    for (std::size_t i = 0; i < options.mutators.size(); ++i) {
        const VoteMutatorSchema& schema = options.mutators[i];
        std::vector<MutatorEditorOption>& list = rows.options.emplace_back();
        for (const VoteMutatorOptionSchema& option : schema.options) {
            list.push_back(MutatorEditorOption{option.label.c_str(), option.type, &option.choices, false});
        }
        rows.editors.push_back(MutatorEditor{
            .key = schema.id,
            .label = schema.label.c_str(),
            .desc_id = static_cast<int>(i),
            .enabled = vote_mutator_offered(schema, game_type),
            .options = list,
        });
    }
    return rows;
}

// The sub-header over the mutators, with the buttons that load a set into them.
void build_mutators_header(UiContext& ctx, FormScroll& scroll, int x, int& y, const VoteOptionsData& options,
                           const MenuFonts& fonts)
{
    const UiId current_id = ui_id("vote_mutators_current");
    const UiId base_id = ui_id("vote_mutators_base");
    const int y0 = y;
    const auto rect =
        form_scroll_row(ctx, scroll, x, y, ctx.scaled(form_sub_header_h), current_id, UiFocusKind::Activate);
    if (ctx.focus_id() == base_id) {
        scroll.focus_rect = UiRect{x, y0, scroll.inner_w, y - y0};
    }
    // Disabled rather than applied as an empty set: "base runs nothing" is not "base unknown".
    const bool base_known = options.base_mutator_decls_present;
    if (!rect) {
        if (base_known) {
            ctx.register_focusable(base_id, UiFocusKind::Activate);
        }
        return;
    }
    form_draw_sub_header(ctx, *rect, "Mutators", fonts.column);

    const int h = skin_px(ctx, skin::widget_h);
    const int by = rect->y + (rect->h - h) / 2;
    const int current_w = skin_button_width(ctx, "CURRENT", fonts.caption, 110.0f);
    const int base_w = skin_button_width(ctx, "BASE", fonts.caption, 90.0f);
    const UiRect current{rect->x + rect->w - ctx.scaled(form_row_inset) - current_w, by, current_w, h};
    const UiRect base{current.x - ctx.scaled(12.0f) - base_w, by, base_w, h};
    if (!ctx.draw) {
        if (ui_hover(ctx, current)) {
            ctx.description().hover(desc_current_mutators);
        }
        else if (ui_hover(ctx, base)) {
            ctx.description().hover(desc_base_mutators);
        }
    }
    if (skin_button(ctx, base, "BASE", fonts.caption, SkinButton::Secondary, base_known, base_id) && base_known) {
        vote_form_use_base_mutators(options);
        ui_play_click();
    }
    if (skin_button(ctx, current, "CURRENT", fonts.caption, SkinButton::Secondary, true, current_id)) {
        vote_form_use_current_mutators(options);
        ui_play_click();
    }
}

void build_level_rows(UiContext& ctx, const UiRect& view, const VoteOptionsData& options, bool is_match,
                      const std::vector<const VoteGametypeInfo*>& gametypes, const MenuFonts& fonts)
{
    VoteForm& form = vote_form();
    const FormFonts ff = form_fonts(fonts);
    const int row_h = skin_px(ctx, skin::row_h);
    const int count = static_cast<int>(gametypes.size());

    // Grays the mutators the vote's game type excludes; taken again after the game type row, so a change shows at once.
    MutatorRows mutators = mutator_rows(options, vote_form_effective_gametype(options, gametypes));
    int content_h = (is_match ? 2 : 1) * row_h + ctx.scaled(form_sub_header_h);
    for (std::size_t i = 0; i < mutators.editors.size() && i < form.mutators.size(); ++i) {
        content_h += mutator_editor_height(ctx, mutators.editors[i], form.mutators[i].enabled);
    }

    FormScroll scroll = form_scroll_begin(ctx, view, content_h, g_form_scroll, row_h * 3, ui_id("vote_form"));
    const int x = view.x;
    int y = scroll.region.start_y();
    const auto line_for = [&](const UiRect& rect, UiId id, const char* label, int desc, bool enabled = true,
                              bool modified = false, UiId reset_id = 0) {
        const FormRow row{
            .rect = rect,
            .id = id,
            .label = label,
            .enabled = enabled,
            .modified = modified,
            .reset_id = reset_id,
            .desc_id = desc,
        };
        return form_row(ctx, row, ff);
    };

    if (is_match) {
        const UiId id = ui_id("vote_team_size");
        if (const auto rect = form_scroll_row(ctx, scroll, x, y, row_h, id, UiFocusKind::Adjustable)) {
            const FormLine line = line_for(*rect, id, "Team size", desc_team_size);
            const std::string text = std::format("{}v{}", form.team_size, form.team_size);
            if (const int step = form_cycler(ctx, line, id, text.c_str(), ff.body); step != 0) {
                form.team_size = wrap_step(form.team_size - 1, step, 8) + 1;
                ui_play_click();
            }
        }
    }

    {
        const UiId id = ui_id("vote_gametype");
        const UiFocusKind kind = count > form_cycler_max_options ? UiFocusKind::Activate : UiFocusKind::Adjustable;
        if (const auto rect = form_scroll_row(ctx, scroll, x, y, row_h, count > 0 ? id : 0, kind)) {
            const FormLine line = line_for(*rect, id, "Game type", desc_gametype, count > 0,
                                           form.gametype_touched && count > 0, ui_id("vote_gametype_reset"));
            if (line.reset) {
                vote_form_reset_gametype(options, is_match);
                ui_play_click();
            }
            if (count == 0) {
                form_value_text(ctx, line, "-", ff.body);
            }
            else {
                std::vector<std::string> names;
                names.reserve(gametypes.size());
                for (const VoteGametypeInfo* gametype : gametypes) {
                    names.push_back(gametype->name);
                }
                int index = form.gametype_index;
                if (form_choice(ctx, line, id, names, index, ff.body)) {
                    vote_form_pick_gametype(index);
                }
            }
        }
    }

    build_mutators_header(ctx, scroll, x, y, options, fonts);

    const uint8_t mutator_gametype = vote_form_effective_gametype(options, gametypes);
    // Settled against the type picked this pass, before the row that shows it.
    if (!ctx.draw) {
        vote_form_sync_score_limit(options, mutator_gametype);
    }
    for (std::size_t i = 0; i < mutators.editors.size() && i < form.mutators.size(); ++i) {
        MutatorEditor& editor = mutators.editors[i];
        editor.enabled = vote_mutator_offered(options.mutators[i], mutator_gametype);
        VoteMutatorSelection& selection = form.mutators[i];
        if (mutator_editor_rows(ctx, scroll, x, y, editor, selection.enabled, selection.options, ff,
                                g_prev_text_focus)
                .changed) {
            // The form is the player's from here on.
            form.mutators_touched = true;
        }
    }

    form_scroll_end(ctx, scroll, g_form_scrolled_to);
}

void build_level_tab(UiContext& ctx, const MenuPanel& panel, const VoteOptionsData& options, const VoteTypeInfo& type,
                     const MenuFonts& fonts)
{
    const bool is_match = type.type == AfVoteType::Match;
    if (!ctx.draw) {
        vote_form_sync_gametype(options, is_match);
    }
    const auto gametypes = vote_selectable_gametypes(options, is_match);
    // The form's game type, which for Match is always a team type.
    const uint8_t gametype = vote_form_selected_gametype(gametypes);

    const FormPanelAreas areas = form_panel_areas(ctx, panel, true);
    const int gap = ctx.scaled(24.0f);
    const int list_w = std::clamp((areas.body.w - gap) * 2 / 5, ctx.scaled(240.0f), ctx.scaled(520.0f));
    const int field_h = skin_px(ctx, skin::widget_h);
    const int x = areas.body.x;
    const int search_y = areas.body.y + ctx.scaled(8.0f);
    build_level_search(ctx, UiRect{x, search_y, list_w, field_h}, options, gametypes, gametype, fonts);
    const int list_y = search_y + field_h + ctx.scaled(12.0f);
    const int field_y = areas.body.y + areas.body.h - field_h;
    const int box_y = field_y - ctx.scaled(8.0f) - field_h;
    build_level_list(ctx, UiRect{x, list_y, list_w, box_y - ctx.scaled(12.0f) - list_y}, options, is_match, gametype,
                     fonts);
    build_manual_level(ctx, UiRect{x, box_y, list_w, field_h}, UiRect{x, field_y, list_w, field_h}, fonts);
    const int rows_x = x + list_w + gap;
    build_level_rows(ctx, form_rows_view(ctx, UiRect{rows_x, areas.body.y, x + areas.body.w - rows_x, areas.body.h}),
                     options, is_match, gametypes, fonts);
    build_description(ctx, areas.desc, &options, type.description, fonts);
    build_vote_footer(ctx, areas.footer, options, type.type, fonts);
}

// ─── The other vote tabs ───

void build_rotation_tab(UiContext& ctx, const MenuPanel& panel, const VoteOptionsData& options, const VoteTab& tab,
                        const MenuFonts& fonts)
{
    VoteForm& form = vote_form();
    const FormFonts ff = form_fonts(fonts);
    const FormPanelAreas areas = form_panel_areas(ctx, panel, true);
    const int row_h = skin_px(ctx, skin::row_h);
    const int count = static_cast<int>(tab.types.size());
    form.rotation_index = std::clamp(form.rotation_index, 0, count - 1);

    const UiRect view = form_rows_view(ctx, areas.body);
    FormScroll scroll = form_scroll_begin(ctx, view, 2 * row_h, g_form_scroll, row_h * 3, ui_id("vote_form"));
    int y = scroll.region.start_y();
    {
        const UiId id = ui_id("vote_rotation_action");
        if (const auto rect = form_scroll_row(ctx, scroll, view.x, y, row_h, id, UiFocusKind::Adjustable)) {
            const FormLine line = form_row(ctx, FormRow{.rect = *rect, .id = id, .label = "Action",
                                                        .desc_id = desc_rotation_action},
                                           ff);
            std::vector<std::string> labels;
            for (const VoteTypeInfo* type : tab.types) {
                labels.emplace_back(type->label);
            }
            form_choice(ctx, line, id, labels, form.rotation_index, ff.body);
        }
    }
    // Only offered on a server that reads the flag.
    if (options.rotation_preserve_supported) {
        const UiId id = ui_id("vote_rotation_preserve");
        if (const auto rect = form_scroll_row(ctx, scroll, view.x, y, row_h, id, UiFocusKind::Adjustable)) {
            const FormRow row{
                .rect = *rect, .id = id, .label = "Preserve game type and mutators", .desc_id = desc_preserve};
            const FormLine line = form_row(ctx, row, ff);
            form_toggle(ctx, line, id, form.rotation_preserve, ff.body);
        }
    }
    form_scroll_end(ctx, scroll, g_form_scrolled_to);

    const VoteTypeInfo* type = vote_tab_type(tab);
    build_description(ctx, areas.desc, &options, type ? type->description : "", fonts);
    if (type) {
        build_vote_footer(ctx, areas.footer, options, type->type, fonts);
    }
}

void build_extend_tab(UiContext& ctx, const MenuPanel& panel, const VoteOptionsData& options, const VoteTypeInfo& type,
                      const MenuFonts& fonts)
{
    const FormFonts ff = form_fonts(fonts);
    const FormPanelAreas areas = form_panel_areas(ctx, panel, true);
    const int row_h = skin_px(ctx, skin::row_h);
    const UiRect view = form_rows_view(ctx, areas.body);
    FormScroll scroll = form_scroll_begin(ctx, view, row_h, g_form_scroll, row_h * 3, ui_id("vote_form"));
    int y = scroll.region.start_y();
    const UiId id = ui_id("vote_extend_minutes");
    if (const auto rect = form_scroll_row(ctx, scroll, view.x, y, row_h, id, UiFocusKind::Text)) {
        const std::string label =
            std::format("Minutes ({}-{})", af_vote_extend_min_minutes, af_vote_extend_max_minutes);
        const FormLine line = form_row(ctx, FormRow{.rect = *rect, .id = id, .label = label.c_str()}, ff);
        const std::string minutes = std::to_string(vote_form().extend_minutes);
        const FormTextSpec spec{.max_len = 2, .filter = ui_char_filter_digit, .hint = ""};
        form_number_text(ctx, line, id, minutes, minutes, spec, ff.body, g_prev_text_focus,
                         [](const std::string& text) { vote_form_set_extend_minutes(text); });
    }
    form_scroll_end(ctx, scroll, g_form_scrolled_to);
    build_description(ctx, areas.desc, &options, type.description, fonts);
    build_vote_footer(ctx, areas.footer, options, type.type, fonts);
}

void build_kick_tab(UiContext& ctx, const MenuPanel& panel, const VoteOptionsData& options, const VoteTypeInfo& type,
                    const MenuFonts& fonts)
{
    VoteForm& form = vote_form();
    const FormPanelAreas areas = form_panel_areas(ctx, panel, true);
    const std::vector<VoteKickCandidate>& candidates = vote_form_kick_candidates(!ctx.draw);
    std::vector<std::string> names;
    names.reserve(candidates.size());
    for (const VoteKickCandidate& candidate : candidates) {
        names.push_back(candidate.name);
    }
    const int picked = build_name_list(ctx, areas.body, names, candidates.empty() ? -1 : form.kick_index,
                                       form.kick_scroll, g_kick_scrolled_to, "vote_kick", "No players to kick",
                                       fonts);
    if (picked >= 0) {
        form.kick_index = picked;
        ui_play_click();
    }
    build_description(ctx, areas.desc, &options, type.description, fonts);
    build_vote_footer(ctx, areas.footer, options, type.type, fonts);
}

void build_plain_tab(UiContext& ctx, const MenuPanel& panel, const VoteOptionsData& options, const VoteTypeInfo& type,
                     const MenuFonts& fonts)
{
    const FormPanelAreas areas = form_panel_areas(ctx, panel, false);
    draw_paragraph(ctx, areas.body, type.description, fonts);
    build_vote_footer(ctx, areas.footer, options, type.type, fonts);
}

// ─── Saved ───

void confirm_delete(int index)
{
    const auto& store = saved_votes_get();
    if (index < 0 || index >= static_cast<int>(store.size())) {
        return;
    }
    ui_play_click();
    g_menu.popup().open("Delete Saved Vote", std::format("Delete \"{}\"?", store[index].name),
                        {{"CANCEL"}, {"DELETE"}}, 0, 0,
                        [index, name = store[index].name](int button, const std::string&) {
                            const auto& now = saved_votes_get();
                            if (button == 1 && index < static_cast<int>(now.size()) && now[index].name == name) {
                                vote_form_saved_delete(index);
                                ui_play_toggle(false);
                            }
                        });
}

void build_saved_list(UiContext& ctx, const UiRect& well, const MenuFonts& fonts)
{
    VoteForm& form = vote_form();
    const auto& store = saved_votes_get();
    const int row_h = skin_list_row_h(ctx, fonts.body);
    if (!ctx.draw) {
        form.saved_hovered = -1;
        // A freshly saved entry, or the selection the tab was re-entered with, is centered once.
        if (std::exchange(form.saved_scroll_to_selection, false) && form.saved_selected >= 0) {
            center_row(form.saved_scroll, form.saved_selected, row_h, well, ctx);
        }
    }
    const SkinScrollList list{
        .count = static_cast<int>(store.size()),
        .row_h = row_h,
        .id_name = "vote_saved_list",
        .row_id_name = "vote_saved",
        .empty_text = store.empty() ? "No saved votes yet." : nullptr,
        .font = fonts.body,
    };
    int deleted = -1;
    skin_scroll_list(ctx, well, list, form.saved_scroll, g_saved_scrolled_to, [&](const SkinScrollListRow& row) {
        const int i = row.index;
        // Previewable but unselectable, so CALL VOTE is never armed with an entry this server can't take.
        const bool callable = vote_form_saved_callable(i);
        if (!row.in_view) {
            if (row.offscreen_activated && callable) {
                form.saved_selected = i;
            }
            return;
        }
        const SkinListRow r = skin_list_row(ctx, row.rect, i == form.saved_selected, row.id);
        const int icon = skin_px(ctx, skin::icon_size);
        const UiRect del{row.rect.x + row.rect.w - ctx.scaled(14.0f) - icon, row.rect.y + (row.rect.h - icon) / 2,
                         icon, icon};
        const bool del_pressed =
            skin_icon_button(ctx, del, "af_close_icon", skin::txt_hint, skin::warn, ui_id("vote_saved_delete", i));
        if (ctx.draw) {
            const rf::Color color = !callable ? skin::txt_dim : r.lit ? skin::txt_hi : skin::txt;
            draw_row_text(ctx, r, store[i].name, row.rect.x + row.rect.w - del.x, color, fonts.body);
            return;
        }
        if (ui_hover(ctx, row.rect, row.id)) {
            form.saved_hovered = i;
        }
        // The delete icon deliberately does not select.
        if (del_pressed) {
            deleted = i;
        }
        else if ((r.clicked || r.activated) && callable) {
            form.saved_selected = i;
            ui_play_click();
        }
    });
    if (deleted >= 0) {
        confirm_delete(deleted);
    }
}

void build_saved_preview(const UiContext& ctx, const UiRect& pane, const VoteOptionsData* options,
                         const MenuFonts& fonts)
{
    if (!ctx.draw) {
        return;
    }
    const auto& store = saved_votes_get();
    const int line_h = rf::gr::get_font_height(fonts.body) + ctx.scaled(6.0f);
    const int index = vote_form_saved_previewed();
    if (index < 0) {
        draw_lines(pane,
                   {store.empty() ? "Set up a Level, Match or Extend vote and press SAVE."
                                  : "Hover a saved vote to preview it."},
                   fonts.body, line_h, skin::ink_hint);
        return;
    }

    const SavedVote& vote = store[index];
    int y = pane.y;
    const SkinText name = skin_fit_text(ctx, vote.name, fonts.name, pane.w);
    rf::gr::set_color(skin::ink_title);
    rf::gr::string(pane.x, y, name.text.c_str(), name.font);
    y += rf::gr::get_font_height(name.font) + ctx.scaled(12.0f);

    const int bottom = pane.y + pane.h;
    const SavedVoteAvailability* availability = vote_form_saved_availability(index);
    if (availability && !availability->callable) {
        std::vector<std::string> warning{"Can't be called on this server:"};
        for (const std::string& reason : availability->reasons) {
            warning.push_back("- " + reason);
        }
        // At most half the pane, so a long reason list never pushes out the printout that identifies the entry.
        const int cap = std::max(line_h, (bottom - y) / 2);
        const int height = std::min(ui_line_block_height(warning, pane.w, fonts.body, line_h, cap), cap);
        draw_lines(UiRect{pane.x, y, pane.w, height}, warning, fonts.body, line_h, skin::ink_alert);
        y += height + ctx.scaled(8.0f);
    }
    draw_lines(UiRect{pane.x, y, pane.w, std::max(line_h, bottom - y)}, vote_form_saved_printout(vote, options),
               fonts.body, line_h, skin::ink_status);
}

void build_saved_tab(UiContext& ctx, const MenuPanel& panel, const VoteOptionsData* options, const MenuFonts& fonts)
{
    if (!ctx.draw) {
        vote_form_saved_refresh(options, g_mode);
    }
    else if (g_mode == VoteFormMode::Loading) {
        // The list needs the blob to say which entries can be called here.
        vote_options_request_if_needed();
    }
    const FormPanelAreas areas = form_panel_areas(ctx, panel, false);
    const int gap = ctx.scaled(24.0f);
    const int list_w = (areas.body.w - gap) * 45 / 100;
    build_saved_list(ctx, UiRect{areas.body.x, areas.body.y, list_w, areas.body.h}, fonts);
    build_saved_preview(ctx, UiRect{areas.body.x + list_w + gap, areas.body.y, areas.body.w - list_w - gap,
                                    areas.body.h},
                        options, fonts);

    const std::array<FooterButton, 3> buttons{{
        {"EDIT", vote_form_saved_can_edit(options, g_mode)},
        {"CANCEL VOTE", vote_form_can_cancel()},
        {"CALL VOTE", vote_form_saved_can_call(options)},
    }};
    switch (build_footer(ctx, areas.footer, buttons, fonts)) {
        case 0:
            vote_form_saved_edit(*options);
            reset_description();
            g_form_scroll = 0.0f;
            ui_play_click();
            break;
        case 1:
            vote_form_cancel();
            ui_play_click();
            break;
        case 2:
            vote_form_saved_call(*options);
            ui_play_click();
            request_close();
            break;
        default:
            break;
    }
}

// ─── Panels ───

void build_status_body(UiContext& ctx, const MenuPanel& panel, const MenuFonts& fonts)
{
    const FormPanelAreas areas = form_panel_areas(ctx, panel, false);
    switch (g_mode) {
        case VoteFormMode::NotSupported:
            draw_message(ctx, areas.body,
                         {"This server does not support menu voting", "(requires Alpine Faction 1.4 or newer)"}, fonts);
            break;
        case VoteFormMode::Loading:
            if (ctx.draw) {
                vote_options_request_if_needed();
            }
            draw_message(ctx, areas.body, {"Loading server vote options..."}, fonts);
            break;
        case VoteFormMode::Form: {
            draw_message(ctx, areas.body, {vote_form_no_tabs_text()}, fonts);
            const std::array<FooterButton, 1> buttons{{{"CANCEL VOTE", vote_form_can_cancel()}}};
            if (build_footer(ctx, areas.footer, buttons, fonts) == 0) {
                vote_form_cancel();
                ui_play_click();
            }
            break;
        }
    }
}

void build_tab(UiContext& ctx, const MenuPanel& panel, int index, const MenuFonts& fonts)
{
    const VoteOptionsData* options = vote_options_get();
    if (index >= static_cast<int>(g_tabs.size()) || !options) {
        return;
    }
    const VoteTab& tab = g_tabs[index];
    const VoteTypeInfo* type = vote_tab_type(tab);
    if (!type) {
        return;
    }
    if (tab.is_rotation) {
        build_rotation_tab(ctx, panel, *options, tab, fonts);
        return;
    }
    switch (type->type) {
        case AfVoteType::Level:
        case AfVoteType::Match:
            build_level_tab(ctx, panel, *options, *type, fonts);
            break;
        case AfVoteType::Extend:
            build_extend_tab(ctx, panel, *options, *type, fonts);
            break;
        case AfVoteType::Kick:
            build_kick_tab(ctx, panel, *options, *type, fonts);
            break;
        default:
            build_plain_tab(ctx, panel, *options, *type, fonts);
            break;
    }
}

// A vote tab, the Saved tab, or the no-tabs message; the form decides which one is open.
class VotePanel final : public ColumnPanel
{
public:
    explicit VotePanel(int action) : action_{action} {}
    [[nodiscard]] bool is_open() const override
    {
        const VoteForm& form = vote_form();
        if (action_ == saved_action) {
            return form.on_saved_tab;
        }
        if (form.on_saved_tab) {
            return false;
        }
        if (action_ == status_action) {
            return g_tabs.empty();
        }
        return form.type_index == action_ && action_ < static_cast<int>(g_tabs.size());
    }
    void open() override
    {
        if (action_ == saved_action) {
            vote_form_select_saved();
        }
        else if (action_ != status_action) {
            vote_form_select_tab(action_);
        }
    }
    // One tab is always picked; picking another is what closes this one.
    void close() override {}
    // Esc leaves the menu.
    bool back() override
    {
        return false;
    }
    void build(UiContext& ctx, const MenuPanel& panel, const MenuFonts& fonts) override
    {
        if (ctx.draw) {
            const char* heading = action_ == saved_action ? "Saved"
                : action_ == status_action                ? "Call Vote"
                : action_ < static_cast<int>(g_tabs.size()) ? g_tabs[action_].label
                                                            : "";
            menu_draw_panel_heading(ctx, panel, string_to_upper(heading).c_str(), fonts.heading);
        }
        if (action_ == saved_action) {
            build_saved_tab(ctx, panel, vote_options_get(), fonts);
        }
        else if (action_ == status_action) {
            build_status_body(ctx, panel, fonts);
        }
        else {
            build_tab(ctx, panel, action_, fonts);
        }
    }
    void hints(const UiContext& ctx, std::vector<MenuHint>& hints) const override
    {
        // Over the game the menu's control closes it too, unless the typing takes the key.
        if (g_host == Host::Game && ctx.focused_text_id() == 0 && !g_close_key.empty()) {
            hints.push_back({g_close_key, "Return"});
        }
    }

private:
    int action_;
};

std::array<VotePanel, vote_form_max_tabs> g_tab_panels{
    VotePanel{0}, VotePanel{1}, VotePanel{2}, VotePanel{3}, VotePanel{4}, VotePanel{5}, VotePanel{6},
};
VotePanel g_saved_panel{saved_action};
VotePanel g_status_panel{status_action};

std::vector<ColumnButton> make_buttons()
{
    std::vector<ColumnButton> buttons;
    for (int i = 0; i < static_cast<int>(g_tabs.size()) && i < vote_form_max_tabs; ++i) {
        buttons.push_back(ColumnButton{i, string_to_upper(g_tabs[i].label), rf::KEY_NONE, true});
    }
    buttons.push_back(ColumnButton{saved_action, "SAVED", rf::KEY_NONE, true});
    return buttons;
}

bool same_tabs(const std::vector<VoteTab>& a, const std::vector<VoteTab>& b)
{
    return std::ranges::equal(a, b, [](const VoteTab& x, const VoteTab& y) {
        return std::string_view{x.label} == std::string_view{y.label};
    });
}

ColumnPanel* current_panel()
{
    if (vote_form().on_saved_tab) {
        return &g_saved_panel;
    }
    if (g_tabs.empty()) {
        return &g_status_panel;
    }
    const int last = std::min(static_cast<int>(g_tabs.size()), vote_form_max_tabs) - 1;
    return &g_tab_panels[std::clamp(vote_form().type_index, 0, last)];
}

// The bind name allocates on the engine heap, so it is resolved again only when the bind changes.
void refresh_close_key()
{
    if (g_host != Host::Game || !rf::local_player) {
        return;
    }
    const int action = get_af_control(rf::AF_ACTION_VOTE_MENU);
    const rf::ControlConfigItem& item = rf::local_player->settings.controls.bindings[action];
    const std::array<int16_t, 3> bind{item.scan_codes[0], item.scan_codes[1], item.mouse_btn_id};
    if (bind != g_close_bind) {
        g_close_bind = bind;
        g_close_key = get_action_bind_name_or(action, "");
    }
}

// Once a frame, before either pass: the form and the tabs follow the server's vote options.
void refresh()
{
    refresh_close_key();
    UiContext& ui = g_menu.ui();
    g_mode = vote_form_mode();
    const VoteOptionsData* options = vote_options_get();
    std::vector<VoteTab> tabs;
    if (g_mode == VoteFormMode::Form && options) {
        if (vote_form_needs_build(*options)) {
            // A typed number would be written into the values the rebuild moves.
            ui.release_text_focus(false);
            reset_description();
        }
        vote_form_ensure(*options);
        vote_form_sync_baseline(*options);
        tabs = vote_form_tabs();
        vote_form_clamp_tab(tabs);
    }
    const bool changed = !same_tabs(tabs, g_tabs);
    g_tabs = std::move(tabs);
    if (changed) {
        g_menu.set_buttons(make_buttons());
        // The form arriving in the placeholder's place is no navigation, so it takes the seat rather than sliding.
        if (g_menu.shown_panel() == &g_status_panel && !vote_form().on_saved_tab) {
            reset_description();
            g_menu.seat_panel(current_panel());
        }
    }
}

void enter(Host host)
{
    g_host = host;
    g_close_pending = false;
    g_prev_text_focus = 0;
    g_form_scroll = 0.0f;
    vote_form_on_open();
    g_tabs.clear();
    reset_description();
    g_menu.enter("CALL VOTE", {}, ColumnButton{back_action, menu_stock_label(rf::strings::back), rf::KEY_B},
                 back_action);
    // Over the game only Esc and the typing reach the menu.
    if (host == Host::Game) {
        g_menu.set_esc_hint("Return", false);
        // No menu is beneath to go back to.
        g_menu.set_strip_clicks(false);
        g_close_key.clear();
        g_close_bind.reset();
    }
    for (int i = 0; i < vote_form_max_tabs; ++i) {
        g_menu.add_panel(&g_tab_panels[i], i);
    }
    g_menu.add_panel(&g_saved_panel, saved_action);
    g_menu.add_panel(&g_status_panel, status_action);
    refresh();
    g_menu.set_buttons(make_buttons());
    g_menu.seat_panel(current_panel());
}

void leave()
{
    g_host = Host::None;
    g_close_pending = false;
    // A forced close discards a half-typed number.
    g_menu.ui().release_text_focus(false);
    g_menu.popup().close();
    vote_form_on_close();
}

void run_action(int action)
{
    if (!g_menu.jump_action()) {
        ui_play_click();
    }
    if (action == back_action) {
        request_close();
        return;
    }
    VotePanel* panel = action == saved_action        ? &g_saved_panel
        : action >= 0 && action < vote_form_max_tabs ? &g_tab_panels[action]
                                                     : nullptr;
    if (!panel || panel->is_open()) {
        return;
    }
    reset_description();
    g_form_scroll = 0.0f;
    g_menu.open_panel(panel);
}

void __cdecl key_callback(int key)
{
    g_menu.key(key);
}

} // namespace

void vote_menu_open()
{
    enter(Host::PauseMenu);
}

bool vote_menu_is_active()
{
    return g_host == Host::PauseMenu;
}

void vote_menu_do_frame(bool paused)
{
    refresh();
    g_menu.step();
    if (!paused) {
        const std::optional<int> action = g_menu.input(key_callback);
        g_prev_text_focus = g_menu.ui().focused_text_id();
        if (action) {
            run_action(*action);
        }
    }
    g_menu.draw();
    if (g_menu.is_closed()) {
        vote_menu_release();
    }
}

void vote_menu_release()
{
    if (g_host == Host::PauseMenu) {
        leave();
    }
}

void vote_menu_open_over_game()
{
    enter(Host::Game);
}

void vote_menu_close_over_game()
{
    if (g_host == Host::Game) {
        leave();
    }
}

bool vote_menu_is_over_game()
{
    return g_host == Host::Game;
}

bool vote_menu_over_game_input()
{
    refresh();
    g_menu.step();
    // The keys come through vote_menu_key, from the game's key handler.
    const std::optional<int> action = g_menu.input(nullptr);
    g_prev_text_focus = g_menu.ui().focused_text_id();
    if (action) {
        run_action(*action);
    }
    return !std::exchange(g_close_pending, false);
}

void vote_menu_over_game_render()
{
    rf::gr::reset_clip();
    menu_draw_modal_dim();
    g_menu.draw(false);
}

void vote_menu_over_game_escape()
{
    g_menu.key(rf::KEY_ESC);
}

bool vote_menu_takes_keys()
{
    return g_host == Host::Game && (g_menu.ui().focused_text_id() != 0 || g_menu.popup().active());
}

void vote_menu_key(int key)
{
    g_menu.key(key);
}

void vote_menu_drop_edits()
{
    g_menu.ui().release_text_focus(false);
    reset_description();
}
