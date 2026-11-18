#include <algorithm>
#include <format>
#include <optional>
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
#include "../misc/settings_registry.h"
#include "../multi/multi.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/input.h"
#include "../rf/player/player.h"

namespace
{

const std::vector<std::string> team_labels{"Auto", "Red", "Blue"};
// A bot whose name and character are no default identity's.
constexpr int custom_identity = -2;

std::vector<ListenServerBot>& roster()
{
    return create_game_setup().bots();
}

const MutatorOptionInfo* join_bots_option(std::string_view name)
{
    const MutatorInfo* info = mutators_find_by_id(MutatorId::JoinBots);
    if (!info) {
        return nullptr;
    }
    const auto it = std::find_if(info->options.begin(), info->options.end(),
        [&](const MutatorOptionInfo& option) { return option.name == name; });
    return it != info->options.end() ? &*it : nullptr;
}

const std::vector<MutatorOptionChoice>& option_choices(std::string_view name)
{
    static const std::vector<MutatorOptionChoice> none;
    const MutatorOptionInfo* option = join_bots_option(name);
    return option ? option->choices : none;
}

int choice_index(const std::vector<MutatorOptionChoice>& choices, std::string_view value)
{
    const auto it = std::find_if(choices.begin(), choices.end(),
        [&](const MutatorOptionChoice& c) { return string_iequals(c.value, value); });
    return it != choices.end() ? static_cast<int>(it - choices.begin()) : -1;
}

std::string choice_label(const std::vector<MutatorOptionChoice>& choices, const std::string& value)
{
    const int index = choice_index(choices, value);
    return index >= 0 ? choices[index].label : value;
}

std::vector<MutatorOptionChoice> g_characters;

const std::vector<MutatorOptionChoice>& characters()
{
    if (g_characters.empty()) {
        if (const ListenServerRule* rule = listen_server_find_rule("force_character.character")) {
            g_characters = listen_server_rule_choices(*rule);
        }
    }
    return g_characters;
}

std::string character_label(const std::string& name)
{
    return name.empty() ? std::string{"any character"} : choice_label(characters(), name);
}

// A default identity, -1 for a random bot, or custom_identity.
int identity_of(const ListenServerBot& bot)
{
    if (bot.name.empty() && bot.character.empty()) {
        return -1;
    }
    const auto identities = default_bot_identities();
    for (size_t i = 0; i < identities.size(); ++i) {
        if (string_iequals(bot.name, identities[i].name) && string_iequals(bot.character, identities[i].character)) {
            return static_cast<int>(i);
        }
    }
    return custom_identity;
}

const char* identity_name(int identity)
{
    const auto identities = default_bot_identities();
    return identity >= 0 && identity < static_cast<int>(identities.size()) ? identities[identity].name : "Random";
}

bool same_bot(const ListenServerBot& a, const ListenServerBot& b)
{
    return a.name == b.name && a.character == b.character && string_iequals(a.skill, b.skill)
        && string_iequals(a.personality, b.personality) && a.team == b.team && a.tuning == b.tuning;
}

std::string bot_label(const ListenServerBot& bot)
{
    return bot.name.empty() ? std::string{"Random"} : bot.name;
}

// A roster entry's presets, or why it does not join.
std::string bot_hint(const ListenServerBot& bot, bool joins)
{
    if (!joins) {
        return "over Max Players";
    }
    return std::format("{}, {}", choice_label(create_game_bot_skills(), bot.skill),
                       choice_label(create_game_bot_personalities(), bot.personality));
}

int preset(int stored, std::string_view option)
{
    const MutatorOptionInfo* info = join_bots_option(option);
    const int count = info ? static_cast<int>(info->choices.size()) : 0;
    if (stored >= 0 && stored < count) {
        return stored;
    }
    return info ? std::min<int>(info->default_choice, count - 1) : -1;
}

} // namespace

const std::vector<MutatorOptionChoice>& create_game_bot_skills()
{
    return option_choices("skill");
}

const std::vector<MutatorOptionChoice>& create_game_bot_personalities()
{
    return option_choices("personality");
}

ListenServerBot create_game_new_bot(int identity)
{
    ListenServerBot bot;
    const auto identities = default_bot_identities();
    if (identity >= 0 && identity < static_cast<int>(identities.size())) {
        bot.name = identities[identity].name;
        bot.character = identities[identity].character;
    }
    CreateGameState& state = create_game();
    state.bot_skill = preset(state.bot_skill, "skill");
    state.bot_personality = preset(state.bot_personality, "personality");
    if (state.bot_skill >= 0) {
        bot.skill = create_game_bot_skills()[state.bot_skill].value;
    }
    if (state.bot_personality >= 0) {
        bot.personality = create_game_bot_personalities()[state.bot_personality].value;
    }
    return bot;
}

void create_game_set_bot_count(int count)
{
    count = std::clamp(count, 0, k_max_server_bots);
    if (count < static_cast<int>(roster().size())) {
        roster().resize(count);
    }
    while (static_cast<int>(roster().size()) < count) {
        roster().push_back(create_game_new_bot(-1));
    }
}

void CreateGameBotsPanel::reset()
{
    open_ = false;
    selected_ = -1;
    reveal_ = false;
    pool_scroll_ = 0.0f;
    pool_scrolled_to_ = 0;
    roster_scroll_ = 0.0f;
    roster_scrolled_to_ = 0;
    edit_ = -1;
    CreateGameState& state = create_game();
    state.bot_skill = preset(state.bot_skill, "skill");
    state.bot_personality = preset(state.bot_personality, "personality");
}

void CreateGameBotsPanel::close()
{
    open_ = false;
    edit_ = -1;
}

void CreateGameBotsPanel::add(int identity)
{
    if (static_cast<int>(roster().size()) >= create_game_bots_limit()) {
        return;
    }
    roster().push_back(create_game_new_bot(identity));
    selected_ = static_cast<int>(roster().size()) - 1;
    reveal_ = true;
}

void CreateGameBotsPanel::move(int to)
{
    const int count = static_cast<int>(roster().size());
    if (selected_ < 0 || selected_ >= count || to < 0 || to >= count) {
        return;
    }
    std::swap(roster()[selected_], roster()[to]);
    selected_ = to;
    reveal_ = true;
}

void CreateGameBotsPanel::remove(int index)
{
    if (index < 0 || index >= static_cast<int>(roster().size())) {
        return;
    }
    roster().erase(roster().begin() + index);
    if (index < selected_) {
        --selected_;
    }
    selected_ = std::min(selected_, static_cast<int>(roster().size()) - 1);
}

bool CreateGameBotsPanel::key(UiContext& ctx, int key)
{
    if (ctx.focused_text_id() != 0 || selected_ < 0 || selected_ >= static_cast<int>(roster().size())) {
        return false;
    }
    if (key == rf::KEY_DELETE) {
        remove(selected_);
        ui_play_click();
        return true;
    }
    return false;
}

void CreateGameBotsPanel::hints(const UiContext& ctx, std::vector<MenuHint>& hints) const
{
    // As key().
    if (ctx.focused_text_id() == 0 && selected_ >= 0 && selected_ < static_cast<int>(roster().size())) {
        hints.push_back({"DEL", "Remove"});
    }
}

void CreateGameBotsPanel::build_pool(UiContext& ctx, const UiRect& well, int font)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const auto identities = default_bot_identities();
    const bool can_add =
        create_game_bots_in_play() && static_cast<int>(roster().size()) < create_game_bots_limit();
    std::optional<int> added;
    const SkinScrollList list{
        .count = 1 + static_cast<int>(identities.size()),
        .row_h = skin_list_row_h(ctx, font),
        .id_name = "cg_bot_pool",
        .row_id_name = "cg_bot_identity",
        .font = font,
    };
    skin_scroll_list(ctx, well, list, pool_scroll_, pool_scrolled_to_, [&](const SkinScrollListRow& row) {
        const int identity = row.index - 1;
        if (!row.in_view) {
            if (row.offscreen_activated) {
                added = identity;
            }
            return;
        }
        const SkinListRow r = skin_list_row(ctx, row.rect, false, row.id);
        if (!ctx.draw) {
            if (r.activated || r.clicked) {
                added = identity;
            }
            return;
        }
        const UiRect& d = r.draw;
        // By character, which a renamed bot keeps.
        const bool on_roster = identity >= 0 && std::any_of(roster().begin(), roster().end(), [&](const auto& bot) {
            return string_iequals(bot.character, identities[identity].character);
        });
        skin_led(ctx, d.x + s(14.0f), d.y + d.h / 2, 7.0f, 22.0f, on_roster);
        const int text_x = d.x + s(32.0f);
        const int text_r = d.x + d.w - s(8.0f);
        const int ty = d.y + (d.h - rf::gr::get_font_height(font)) / 2;
        const std::string name = identity_name(identity);
        skin_draw_text(text_x, ty, name, font, !can_add ? skin::txt_dim : r.lit ? skin::txt_hi : skin::txt);
        const int name_r = text_x + rf::gr::get_string_size(name, font).first + s(16.0f);
        const std::string hint = character_label(identity >= 0 ? identities[identity].character : "");
        rf::gr::set_color(skin::txt_hint);
        rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, text_r, ty,
                               ui_fit_end(hint, std::max(s(20.0f), text_r - name_r), font).c_str(), font);
    });
    if (added && can_add) {
        add(*added);
        ui_play_click();
    }
}

void CreateGameBotsPanel::build_roster(UiContext& ctx, const UiRect& well, int font)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const bool allowed = create_game_bots_in_play();
    const bool teams = create_game_teams_in_play();
    const int limit = create_game_bots_limit();
    const int count = allowed ? static_cast<int>(roster().size()) : 0;
    // Without a team game type there is no team to pick, so the name takes the cycler's room.
    int team_w = 0;
    if (teams) {
        for (const std::string& label : team_labels) {
            team_w = std::max(team_w, rf::gr::get_string_size(label, font).first);
        }
        team_w = skin_cycler_width(ctx, team_w);
    }
    // Under the rows, a line each.
    std::string note;
    const auto add_note = [&](std::string_view line) {
        if (!note.empty()) {
            note += '\n';
        }
        note += line;
    };
    if (!allowed) {
        add_note("Bots can't play this rotation");
    }
    else {
        if (roster().empty()) {
            add_note("Click a bot to add it");
        }
        // A loaded config's profiles past its server_bots, which the panel does not edit.
        if (const int pool = create_game_setup().num_pool_bots(); pool > 0) {
            add_note(std::format("{} more kept for Join Bots", pool));
        }
        if (!teams) {
            add_note("No team game types");
        }
    }
    // The names come first; the presets take what is left, if it is enough to read.
    int name_w = 0;
    if (ctx.draw) {
        for (int i = 0; i < count; ++i) {
            name_w = std::max(name_w, rf::gr::get_string_size(bot_label(roster()[i]), font).first);
        }
    }
    std::optional<int> picked;
    std::optional<int> removed;
    std::optional<int> edited;
    std::optional<UiRect> sel_rect;
    const SkinScrollList list{
        .count = count,
        .row_h = skin_list_row_h(ctx, font),
        .id_name = "cg_bot_roster",
        .row_id_name = "cg_bot",
        .empty_text = note.empty() ? nullptr : note.c_str(),
        .font = font,
    };
    skin_scroll_list(ctx, well, list, roster_scroll_, roster_scrolled_to_, [&](const SkinScrollListRow& row) {
        const int i = row.index;
        ListenServerBot& bot = roster()[i];
        if (i == selected_) {
            sel_rect = row.rect;
        }
        const CreateGameEntry entry = create_game_entry(ctx, row, team_w);
        const UiId team_id = ui_id("cg_bot_team", i);
        const UiId edit_id = ui_id("cg_bot_edit", i);
        if (!row.in_view) {
            if (row.offscreen_activated) {
                picked = i;
            }
            ctx.register_focusable(edit_id, UiFocusKind::Activate);
            if (teams) {
                ctx.register_focusable(team_id, UiFocusKind::Adjustable);
            }
            return;
        }
        const SkinListRow r = skin_list_row(ctx, row.rect, i == selected_, row.id);
        if (teams) {
            if (const int step = skin_cycler(ctx, entry.cycler, team_labels[bot.team + 1].c_str(), font, true,
                                             team_id, entry.on_cycler);
                step != 0 && !ctx.draw) {
                bot.team = wrap_step(bot.team + 1, step, 3) - 1;
                ui_play_click();
            }
        }
        if (create_game_edit_button(ctx, entry.edit, identity_of(bot) == custom_identity || !bot.tuning.empty(),
                                    false, edit_id)) {
            edited = i;
        }
        if (!ctx.draw) {
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
        int text_x = create_game_draw_entry_index(ctx, d, i, font);
        if (teams && bot.team >= 0) {
            const int swatch_w = s(6.0f);
            rf::gr::set_color(bot.team == rf::TEAM_RED ? skin::team_red : skin::team_blue);
            rf::gr::rect(text_x, d.y + s(8.0f), swatch_w, d.h - s(16.0f));
            text_x += swatch_w + s(8.0f);
        }
        const bool joins = i < limit;
        const int text_r = ui_draw_rect(ctx, entry.edit).x - s(12.0f);
        const int span = std::max(0, text_r - text_x);
        const int shown_name_w = std::min(name_w, span);
        skin_draw_text(text_x, ty, ui_fit_end(bot_label(bot), shown_name_w, font), font,
                       !joins ? skin::txt_dim : r.lit ? skin::txt_hi : skin::txt);
        if (const int hint_w = span - shown_name_w - s(12.0f); hint_w >= s(40.0f)) {
            rf::gr::set_color(!joins ? skin::warn : skin::txt_hint);
            const std::string hint = ui_fit_end(bot_hint(bot, joins), hint_w, font);
            rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, text_r, ty, hint.c_str(), font);
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
        selected_ = *edited;
        edit_ = *edited;
        ui_play_click();
        return;
    }
    else if (picked) {
        selected_ = *picked;
        ui_play_click();
    }
    if (std::exchange(reveal_, false) && sel_rect) {
        create_game_reveal(ctx, well, *sel_rect, roster_scroll_);
    }
}

void CreateGameBotsPanel::build(UiContext& ctx, const MenuPanel& panel, const MenuFonts& fonts)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const int font = fonts.body;
    CreateGameState& state = create_game();
    if (ctx.draw) {
        menu_draw_panel_heading(ctx, panel, "BOTS", fonts.heading);
    }
    const bool allowed = create_game_bots_in_play();
    const std::string count_text = std::format("{} / {} bots", roster().size(), create_game_bots_limit());
    const CreateGamePanes panes = create_game_panes(ctx, panel, "ROSTER", count_text, fonts);
    const UiRect& bar = panes.bar;

    const std::vector<MutatorOptionChoice>& skills = create_game_bot_skills();
    const std::vector<MutatorOptionChoice>& personalities = create_game_bot_personalities();
    const int preset_gap = s(12.0f);
    const int preset_w = (bar.w - preset_gap) / 2;
    const UiRect skill_rect{bar.x, bar.y, preset_w, bar.h};
    const UiRect style_rect{bar.x + preset_w + preset_gap, bar.y, bar.w - preset_w - preset_gap, bar.h};
    const std::string skill_text =
        state.bot_skill >= 0 ? std::format("Skill: {}", skills[state.bot_skill].label) : std::string{"Skill"};
    const std::string style_text = state.bot_personality >= 0
        ? std::format("Style: {}", personalities[state.bot_personality].label)
        : std::string{"Style"};
    if (const int step = skin_cycler(ctx, skill_rect, skill_text.c_str(), font, allowed && !skills.empty(),
                                     ui_id("cg_bot_skill"), ui_hover(ctx, skill_rect));
        step != 0 && !ctx.draw) {
        state.bot_skill = wrap_step(state.bot_skill, step, static_cast<int>(skills.size()));
        ui_play_click();
    }
    if (const int step = skin_cycler(ctx, style_rect, style_text.c_str(), font, allowed && !personalities.empty(),
                                     ui_id("cg_bot_style"), ui_hover(ctx, style_rect));
        step != 0 && !ctx.draw) {
        state.bot_personality = wrap_step(state.bot_personality, step, static_cast<int>(personalities.size()));
        ui_play_click();
    }

    build_pool(ctx, panes.left, font);
    build_roster(ctx, panes.right, font);

    if (skin_button(ctx, panes.left_footer, "Apply Skill and Style to All", font, SkinButton::Secondary,
                    allowed && !roster().empty() && state.bot_skill >= 0 && state.bot_personality >= 0,
                    ui_id("cg_bots_apply"))) {
        for (ListenServerBot& bot : roster()) {
            bot.skill = skills[state.bot_skill].value;
            bot.personality = personalities[state.bot_personality].value;
        }
        ui_play_click();
    }
    switch (create_game_list_buttons(ctx, panes.right_footer, selected_, static_cast<int>(roster().size()), allowed,
                                     font, "cg_bot_op")) {
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
            roster().clear();
            selected_ = -1;
            break;
        case CreateGameListOp::None:
            break;
    }
}

// ─── A bot's own settings ───

bool CreateGameBotsPanel::modal_key(UiContext& ctx, int key)
{
    if ((key & rf::KEY_MASK) != rf::KEY_ESC) {
        return false;
    }
    ctx.release_text_focus(true);
    edit_ = -1;
    ui_play_click();
    return true;
}

void CreateGameBotsPanel::build_modal(UiContext& ctx)
{
    if (edit_ < 0 || edit_ >= static_cast<int>(roster().size())) {
        edit_ = -1;
        return;
    }
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const float scale = menu_screen_scale();
    const FormFonts fonts{menu_font(false, 20.0f, scale), menu_font(true, 20.0f, scale),
                          menu_font(false, 17.0f, scale)};
    ListenServerBot& bot = roster()[edit_];
    const int identity = identity_of(bot);

    UiRect dialog;
    const UiRect area = create_game_dialog(ctx, 820, 560, "Bot Settings", dialog);
    const std::string note =
        std::format("Bot {} of the roster. A random name or character is picked as it joins.", edit_ + 1);
    const int note_h = create_game_dialog_note(ctx, area, note, fonts.body);
    const int btn_h = s(44.0f);
    const int btn_y = dialog.y + dialog.h - s(24.0f) - btn_h;
    const int field_y = area.y + note_h + s(8.0f);
    const UiRect field{area.x, field_y, area.w, btn_y - s(14.0f) - field_y};
    if (ctx.draw) {
        skin_steel_field(ctx, field.x, field.y, field.w, field.h);
    }

    const int row_h = skin_px(ctx, skin::row_h);
    const int x = field.x + s(8.0f);
    const int w = field.w - s(16.0f);
    int y = field.y + s(8.0f);
    const auto next_line = [&](UiId id, const char* label, bool enabled = true) {
        const UiRect rect{x, y, w, row_h};
        y += row_h;
        return form_row(ctx, FormRow{.rect = rect, .id = id, .label = label, .enabled = enabled}, fonts);
    };

    {
        const UiId id = ui_id("cg_bot_edit_identity");
        const FormLine line = next_line(id, "Identity");
        std::vector<std::string> items{"Random"};
        for (const DefaultBotIdentity& entry : default_bot_identities()) {
            items.emplace_back(entry.name);
        }
        if (identity == custom_identity) {
            items.emplace_back("Custom");
        }
        int index = identity == custom_identity ? static_cast<int>(items.size()) - 1 : identity + 1;
        if (form_choice(ctx, line, id, items, index, fonts.body)) {
            const ListenServerBot picked = create_game_new_bot(index - 1);
            if (index - 1 < static_cast<int>(default_bot_identities().size())) {
                bot.name = picked.name;
                bot.character = picked.character;
            }
            ctx.release_text_focus(false);
        }
    }
    {
        const UiId id = ui_id("cg_bot_edit_name");
        if (!ctx.draw && ctx.focused_text_id() != id) {
            name_edit_ = bot.name;
        }
        const FormLine line = next_line(id, "Name");
        const FormTextSpec spec{.max_len = player_name_max_len, .filter = player_name_char_allowed, .hint = "Random"};
        const int index = edit_;
        form_text(ctx, line, id, name_edit_, spec, fonts.body, create_game().prev_text_focus, [this, index] {
            if (index < static_cast<int>(roster().size())) {
                roster()[index].name = std::string{trim(name_edit_)};
            }
        });
    }
    {
        const UiId id = ui_id("cg_bot_edit_character");
        const FormLine line = next_line(id, "Character");
        const std::vector<MutatorOptionChoice>& list = characters();
        std::vector<std::string> items{"Random"};
        for (const MutatorOptionChoice& c : list) {
            items.push_back(c.label);
        }
        int index = 0;
        if (!bot.character.empty()) {
            const int found = choice_index(list, bot.character);
            if (found < 0) {
                items.push_back(bot.character);
            }
            index = found >= 0 ? found + 1 : static_cast<int>(items.size()) - 1;
        }
        if (form_choice(ctx, line, id, items, index, fonts.body)) {
            if (index == 0) {
                bot.character.clear();
            }
            else if (index - 1 < static_cast<int>(list.size())) {
                bot.character = list[index - 1].value;
            }
        }
    }
    const auto preset_row = [&](const char* id_name, const char* label, const std::vector<MutatorOptionChoice>& list,
                                std::string& value) {
        const UiId id = ui_id(id_name);
        const FormLine line = next_line(id, label, !list.empty());
        std::vector<std::string> items;
        for (const MutatorOptionChoice& c : list) {
            items.push_back(c.label);
        }
        int index = choice_index(list, value);
        if (index < 0) {
            items.push_back(value);
            index = static_cast<int>(items.size()) - 1;
        }
        if (form_choice(ctx, line, id, items, index, fonts.body) && index < static_cast<int>(list.size())) {
            value = list[index].value;
        }
    };
    preset_row("cg_bot_edit_skill", "Skill", create_game_bot_skills(), bot.skill);
    preset_row("cg_bot_edit_personality", "Personality", create_game_bot_personalities(), bot.personality);
    {
        const UiId id = ui_id("cg_bot_edit_team");
        const bool teams = create_game_teams_in_play();
        const FormLine line = next_line(id, teams ? "Team" : "Team (no team game types)", teams);
        int index = bot.team + 1;
        if (form_choice(ctx, line, id, team_labels, index, fonts.body)) {
            bot.team = index - 1;
        }
    }

    const int btn_w = s(200.0f);
    const UiRect close_rect{dialog.x + dialog.w - s(24.0f) - btn_w, btn_y, btn_w, btn_h};
    const UiRect reset_rect{close_rect.x - s(16.0f) - btn_w, btn_y, btn_w, btn_h};
    ListenServerBot fresh = create_game_new_bot(identity >= 0 ? identity : -1);
    if (skin_button(ctx, reset_rect, "RESET", fonts.body_bold, SkinButton::Secondary, !same_bot(bot, fresh),
                    ui_id("cg_bot_edit_reset"))) {
        ctx.release_text_focus(false);
        bot = std::move(fresh);
        ui_play_click();
    }
    if (skin_button(ctx, close_rect, "CLOSE", fonts.body_bold, SkinButton::Secondary, true,
                    ui_id("cg_bot_edit_close"))) {
        ctx.release_text_focus(true);
        edit_ = -1;
        ui_play_click();
    }
}
