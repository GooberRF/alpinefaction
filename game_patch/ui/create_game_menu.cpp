#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include <common/utils/string-utils.h>
#include "create_game_menu.h"
#include "create_game_internal.h"
#include "column_menu.h"
#include "form_view.h"
#include "menu_shell.h"
#include "mutator_editors.h"
#include "settings_skin.h"
#include "ui_kit.h"
#include "../misc/alpine_settings.h"
#include "../multi/gametype.h"
#include "../multi/map_thumbnails.h"
#include "../multi/multi.h"
#include "../rf/gameseq.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/input.h"
#include "../rf/localize.h"
#include "../rf/os/console.h"
#include "../rf/sound/sound.h"
#include "../rf/ui.h"
#include "../sound/sound.h"

namespace
{

enum class Action
{
    Options,
    Maps,
    Bots,
    Mutators,
    Start,
    Back,
};

// Description ids of the rows that are not rules, whose ids are their index in the rule table.
enum CustomDescription
{
    desc_setup = 1000,
    desc_server_name,
    desc_password_text,
    desc_max_players,
    desc_lan_only,
    desc_game_type,
    desc_bots,
    // Plus the mutator's index in the registry.
    desc_mutators = 2000,
};

constexpr std::pair<int, const char*> custom_descriptions[] = {
    {desc_setup, "Setups saved in saved\\servers, in the dedicated server's config format."},
    {desc_server_name, "The name the server list shows."},
    {desc_password_text, "The password players type to join, up to 16 characters; none leaves the server open."},
    {desc_max_players, "The most players the server takes, the host included."},
    {desc_lan_only, "Keep the server off the internet server list."},
    {desc_game_type, "The game type a map added to the rotation next runs, and the map list's filter."},
    {desc_bots, "Server bots that join the game; BOTS sets each one up."},
};

constexpr int max_warnings_shown = 5;

ColumnMenu g_menu;
CreateGameState g_state;
CreateGameRowsPanel g_options;
CreateGameMapsPanel g_maps;
CreateGameBotsPanel g_bots;
CreateGameMutatorsPanel g_mutators;
// The first visit of a run starts from the default setup; later visits keep the setup as it was left.
bool g_loaded = false;
std::vector<std::string> g_saved_setups;
int g_saved_index = -1;
// The saved setup the form was last loaded from or saved to; empty for one that came from neither.
std::string g_setup_name;
// The boxes' text, refreshed from the setup while they are not being edited.
std::string g_name_text;
std::string g_password_text;
std::map<const ListenServerRule*, std::vector<MutatorOptionChoice>> g_rule_choices;
std::map<UiId, std::string> g_rule_texts;
// Level names read from the files, keyed by lowercase filename.
std::map<std::string, std::string> g_level_names;

// A number a box commits from a later pass, written into the setup once it lands.
struct NumberCell
{
    float value = 0.0f;
    float shown = 0.0f;
    std::function<void(float)> write;
};
std::map<UiId, NumberCell> g_numbers;

// A mutator's rows' values, which must stay where they are while a box edits one.
struct MutatorCell
{
    int scope = ListenServerSetup::base_scope;
    // Looked up on each use: a hosted level's start rebuilds the registry.
    std::optional<MutatorId> id;
    bool on = false;
    bool on_shown = false;
    std::vector<MutatorEditorValue> values;
    std::vector<MutatorEditorValue> shown;
    std::vector<std::vector<std::string>> choice_labels;
};
std::map<int, MutatorCell> g_mutator_cells;

int action_id(Action action)
{
    return static_cast<int>(action);
}

// What boxes edit, which a new setup or a new visit starts without.
void drop_cells()
{
    g_numbers.clear();
    g_mutator_cells.clear();
    g_rule_texts.clear();
}

// ─── Typed values ───

void flush_number(NumberCell& cell)
{
    if (cell.value != cell.shown && cell.write) {
        cell.shown = cell.value;
        cell.write(cell.value);
    }
}

float& number_cell(UiId id, float current, std::function<void(float)> write)
{
    NumberCell& cell = g_numbers[id];
    flush_number(cell);
    cell.write = std::move(write);
    cell.value = current;
    cell.shown = current;
    return cell.value;
}

// A stepper in the cell `id`: its arrows write at once, a typed value once it is committed.
void number_row(UiContext& ctx, const FormLine& line, UiId id, float current, float min_value, float max_value,
                float step, const UiNumberFormat& fmt, bool integral, int font, std::function<void(float)> write)
{
    float& value = number_cell(id, current, std::move(write));
    if (form_stepper(ctx, line, id, value, min_value, max_value, step, fmt, font, integral)) {
        flush_number(g_numbers[id]);
    }
}

MutatorEditorValue editor_value(const MutatorOptionInfo& option, const MutatorOptionValue& value)
{
    MutatorEditorValue out;
    switch (option.type) {
        case MutatorOptionType::Bool:
            out.bool_value = std::holds_alternative<bool>(value) && std::get<bool>(value);
            break;
        case MutatorOptionType::Int:
            out.int_value = std::holds_alternative<int32_t>(value) ? std::get<int32_t>(value) : option.default_int;
            break;
        case MutatorOptionType::Float:
            out.float_value = std::holds_alternative<float>(value) ? std::get<float>(value) : option.default_float;
            break;
        case MutatorOptionType::Choice: {
            out.choice_index = option.default_choice;
            if (const std::string* name = std::get_if<std::string>(&value)) {
                for (size_t i = 0; i < option.choices.size(); ++i) {
                    if (string_iequals(option.choices[i].value, *name)) {
                        out.choice_index = static_cast<uint8_t>(i);
                    }
                }
            }
            break;
        }
        default:
            break;
    }
    return out;
}

MutatorOptionValue option_value(const MutatorOptionInfo& option, const MutatorEditorValue& value)
{
    switch (option.type) {
        case MutatorOptionType::Bool:
            return value.bool_value;
        case MutatorOptionType::Int:
            return value.int_value;
        case MutatorOptionType::Float:
            return value.float_value;
        case MutatorOptionType::Choice:
            return value.choice_index < option.choices.size() ? option.choices[value.choice_index].value
                                                              : std::string{};
        default:
            return std::string{};
    }
}

bool same_value(const MutatorOptionInfo& option, const MutatorEditorValue& a, const MutatorEditorValue& b)
{
    switch (option.type) {
        case MutatorOptionType::Bool:
            return a.bool_value == b.bool_value;
        case MutatorOptionType::Int:
            return a.int_value == b.int_value;
        case MutatorOptionType::Float:
            return a.float_value == b.float_value;
        case MutatorOptionType::Choice:
            return a.choice_index == b.choice_index;
        default:
            return true;
    }
}

bool mutator_on(int scope, const MutatorInfo& info)
{
    if (scope == ListenServerSetup::base_scope) {
        return create_game_setup().own_mutator(scope, info.name) == true;
    }
    const auto& declarations = create_game_setup().effective_rules(scope).mutators.declarations;
    return std::any_of(declarations.begin(), declarations.end(),
        [&](const MutatorDeclaration& decl) { return string_iequals(decl.name, info.name); });
}

bool mutator_listed(const MutatorInfo& info, int scope)
{
    // Not offered, but listed in a scope that declares them, so a loaded setup's can be turned off or reset.
    constexpr MutatorId hidden[] = {
        MutatorId::Gibbing,
        MutatorId::ScoreLimitOverride,
        MutatorId::IdealPlayerCountOverride,
        MutatorId::JoinBots,
    };
    if (std::ranges::find(hidden, info.id) != std::end(hidden)
        && !create_game_setup().own_mutator(scope, info.name).has_value()) {
        return false;
    }
    return scope == ListenServerSetup::base_scope
        || mutator_gametype_mask_allows(info.valid_gametype_mask,
                                        static_cast<uint8_t>(create_game_setup().game_type(scope)));
}

void mark_clean(MutatorCell& cell)
{
    cell.on_shown = cell.on;
    cell.shown = cell.values;
}

void flush_mutator(MutatorCell& cell)
{
    const MutatorInfo* found = cell.id ? mutators_find_by_id(*cell.id) : nullptr;
    if (!found) {
        return;
    }
    const MutatorInfo& info = *found;
    if (cell.on != cell.on_shown) {
        cell.on_shown = cell.on;
        std::optional<bool> own;
        if (cell.scope == ListenServerSetup::base_scope) {
            own = cell.on ? std::optional{true} : std::nullopt;
        }
        else {
            // A level only declares what differs from the base.
            const bool base_on = create_game_setup().own_mutator(ListenServerSetup::base_scope, info.name) == true;
            if (cell.on != base_on) {
                own = cell.on;
            }
        }
        create_game_setup().set_mutator(cell.scope, info.name, own);
    }
    for (size_t i = 0; i < cell.values.size() && i < info.options.size(); ++i) {
        if (!same_value(info.options[i], cell.values[i], cell.shown[i])) {
            cell.shown[i] = cell.values[i];
            create_game_setup().set_mutator_option(cell.scope, info.name, info.options[i].name,
                                                   option_value(info.options[i], cell.values[i]));
        }
    }
}

MutatorCell& mutator_cell(int key, int scope, const MutatorInfo& info)
{
    MutatorCell& cell = g_mutator_cells[key];
    flush_mutator(cell);
    if (cell.id != info.id || cell.scope != scope || cell.choice_labels.size() != info.options.size()) {
        cell = MutatorCell{};
        cell.scope = scope;
        cell.id = info.id;
        cell.values.resize(info.options.size());
        cell.shown.resize(info.options.size());
        for (const MutatorOptionInfo& option : info.options) {
            std::vector<std::string>& labels = cell.choice_labels.emplace_back();
            for (const MutatorOptionChoice& choice : option.choices) {
                labels.push_back(choice.label);
            }
        }
    }
    cell.on = mutator_on(scope, info);
    for (size_t i = 0; i < info.options.size(); ++i) {
        const MutatorOptionInfo& option = info.options[i];
        cell.values[i] =
            editor_value(option, create_game_setup().effective_mutator_option(scope, info.name, option.name));
    }
    mark_clean(cell);
    return cell;
}

std::vector<MutatorEditorOption> mutator_options(const MutatorCell& cell, const MutatorInfo& info, int scope)
{
    std::vector<MutatorEditorOption> options;
    for (size_t i = 0; i < info.options.size(); ++i) {
        const MutatorOptionInfo& option = info.options[i];
        // A level's declaration replaces the base one whole, so only the base sets options one by one.
        const bool own = scope == ListenServerSetup::base_scope
            && create_game_setup().own_mutator_option(scope, info.name, option.name).has_value();
        options.push_back(MutatorEditorOption{option.label.c_str(), option.type, &cell.choice_labels[i], own});
    }
    return options;
}

int mutator_key(int scope, size_t index)
{
    return (scope + 1) * 64 + static_cast<int>(index);
}

// ─── Rule rows ───

int rule_index(const ListenServerRule& rule)
{
    return static_cast<int>(&rule - listen_server_rules().data());
}

bool rule_row_enabled(int scope, const ListenServerRule& rule)
{
    const ListenServerRule* gate = rule.enabled_by ? listen_server_find_rule(rule.enabled_by) : nullptr;
    if (!gate || rule.off_value) {
        return true;
    }
    const auto value = create_game_setup().effective_rule(scope, *gate);
    const bool* on = value ? std::get_if<bool>(&*value) : nullptr;
    return on && *on;
}

// A hidden row is listed in a scope that sets it, so a loaded value shows and can be reset. A row with an off value
// stands for its toggle.
bool rule_listed(int scope, const ListenServerRule& rule)
{
    if (!rule.hidden) {
        return true;
    }
    const bool stood_for = std::ranges::any_of(listen_server_rules(), [&](const ListenServerRule& row) {
        return row.off_value && row.enabled_by && std::strcmp(row.enabled_by, rule.key) == 0;
    });
    return !stood_for && create_game_setup().own_rule(scope, rule).has_value();
}

bool rule_modified(int scope, const ListenServerRule& rule)
{
    const ListenServerRule* gate = rule.off_value ? listen_server_find_rule(rule.enabled_by) : nullptr;
    return create_game_setup().own_rule(scope, rule) || (gate && create_game_setup().own_rule(scope, *gate));
}

const std::vector<MutatorOptionChoice>& rule_choices(const ListenServerRule& rule)
{
    auto it = g_rule_choices.find(&rule);
    if (it == g_rule_choices.end()) {
        it = g_rule_choices.emplace(&rule, listen_server_rule_choices(rule)).first;
    }
    return it->second;
}

UiNumberFormat rule_format(const ListenServerRule& rule)
{
    if (rule.widget == ListenServerRuleWidget::Int || rule.step >= 1.0f) {
        return UiNumberFormat{};
    }
    const float tenths = rule.step * 10.0f;
    return UiNumberFormat{std::fabs(tenths - std::round(tenths)) < 1.0e-3f ? 1 : 2, 1.0f, ""};
}

float rule_number(const ListenServerRuleValue& value)
{
    if (const int32_t* i = std::get_if<int32_t>(&value)) {
        return static_cast<float>(*i);
    }
    const float* f = std::get_if<float>(&value);
    return f ? *f : 0.0f;
}

template<typename Fn>
void for_each_shown_rule(int scope, CreateGameRuleFilter shown, bool basic_groups_first, Fn&& fn)
{
    std::vector<const ListenServerRule*> rows;
    for (const ListenServerRule& rule : listen_server_rules()) {
        if (rule_listed(scope, rule) && shown(rule, scope)) {
            rows.push_back(&rule);
        }
    }
    // The groups in the order they first come up, each listed once.
    std::vector<std::string_view> groups;
    const auto group_rank = [&](const ListenServerRule* rule) {
        return std::ranges::find(groups, std::string_view{rule->group}) - groups.begin();
    };
    for (const bool basic_pass : {true, false}) {
        if (basic_pass && !basic_groups_first) {
            continue;
        }
        for (const ListenServerRule* rule : rows) {
            if ((rule->basic || !basic_pass) && group_rank(rule) == std::ssize(groups)) {
                groups.emplace_back(rule->group);
            }
        }
    }
    std::ranges::stable_sort(rows, std::less{},
                             [&](const ListenServerRule* rule) { return std::pair{group_rank(rule), !rule->basic}; });
    const char* group = nullptr;
    for (const ListenServerRule* rule : rows) {
        const bool heading = !group || std::strcmp(group, rule->group) != 0;
        group = rule->group;
        fn(*rule, heading);
    }
}

void build_rule_row(UiContext& ctx, FormScroll& scroll, int x, int& y, int scope, const ListenServerRule& rule,
                    const FormFonts& fonts)
{
    const int index = rule_index(rule);
    const int key = (scope + 1) * 1000 + index;
    const UiId id = ui_id("cg_rule", key);
    const bool enabled = rule_row_enabled(scope, rule);
    const std::optional<ListenServerRuleValue> value = create_game_setup().effective_rule(scope, rule);
    const bool number = rule.widget == ListenServerRuleWidget::Int || rule.widget == ListenServerRuleWidget::Float;

    std::vector<std::string> items;
    int choice = 0;
    bool choice_default = false;
    if (rule.widget == ListenServerRuleWidget::Choice) {
        const std::vector<MutatorOptionChoice>& choices = rule_choices(rule);
        const std::string* current = value ? std::get_if<std::string>(&*value) : nullptr;
        const auto found = std::find_if(choices.begin(), choices.end(), [&](const MutatorOptionChoice& c) {
            return current && string_iequals(c.value, *current);
        });
        // The level's own pick: no value the list offers.
        choice_default = found == choices.end();
        if (choice_default) {
            items.emplace_back("Default");
        }
        for (const MutatorOptionChoice& c : choices) {
            items.push_back(c.label);
        }
        choice = choice_default ? 0 : static_cast<int>(found - choices.begin());
    }
    UiFocusKind kind = UiFocusKind::Adjustable;
    if (rule.widget == ListenServerRuleWidget::Text || (number && !value)) {
        kind = UiFocusKind::Text;
    }
    else if (static_cast<int>(items.size()) > form_cycler_max_options) {
        kind = UiFocusKind::Activate;
    }

    const auto rect = form_scroll_row(ctx, scroll, x, y, skin_px(ctx, skin::row_h), enabled ? id : 0, kind);
    if (!rect) {
        return;
    }
    const FormRow row{
        .rect = *rect,
        .id = id,
        .label = rule.label,
        .enabled = enabled,
        .modified = rule_modified(scope, rule),
        .reset_id = ui_id("cg_rule_reset", key),
        .desc_id = index,
    };
    const FormLine line = form_row(ctx, row, fonts);
    if (line.reset) {
        create_game_setup().unset_rule(scope, rule);
        ui_play_click();
        return;
    }

    switch (rule.widget) {
        case ListenServerRuleWidget::Toggle: {
            const bool* current = value ? std::get_if<bool>(&*value) : nullptr;
            bool on = current && *current;
            if (form_toggle(ctx, line, id, on, fonts.body)) {
                create_game_setup().set_rule(scope, rule, on);
            }
            break;
        }
        case ListenServerRuleWidget::Int:
        case ListenServerRuleWidget::Float: {
            const bool integral = rule.widget == ListenServerRuleWidget::Int;
            const ListenServerRule* target = &rule;
            const auto write = [scope, target, integral](float v) {
                const ListenServerRuleValue stored = integral
                    ? ListenServerRuleValue{static_cast<int32_t>(std::lround(v))}
                    : ListenServerRuleValue{v};
                g_state.setup.set_rule(scope, *target, stored);
            };
            if (value) {
                number_row(ctx, line, id, rule_number(*value), rule.min, rule.max, rule.step, rule_format(rule),
                           integral, fonts.body, write);
                break;
            }
            const FormTextSpec spec{.max_len = 12, .filter = ui_char_filter_numeric, .hint = ""};
            form_number_text(ctx, line, id, "Default", "", spec, fonts.body, g_state.prev_text_focus,
                             [write, min = rule.min, max = rule.max](const std::string& text) {
                                 try {
                                     const float parsed = std::stof(text);
                                     if (std::isfinite(parsed)) {
                                         write(std::clamp(parsed, min, max));
                                     }
                                 }
                                 catch (const std::exception&) {
                                     // Not a number: the level keeps its value.
                                 }
                             });
            break;
        }
        case ListenServerRuleWidget::Choice:
            if (form_choice(ctx, line, id, items, choice, fonts.body)) {
                if (choice_default && choice == 0) {
                    create_game_setup().unset_rule(scope, rule);
                }
                else {
                    const int picked = choice - (choice_default ? 1 : 0);
                    create_game_setup().set_rule(scope, rule, rule_choices(rule)[picked].value);
                }
            }
            break;
        case ListenServerRuleWidget::Text: {
            std::string& text = g_rule_texts[id];
            if (!ctx.draw && ctx.focused_text_id() != id) {
                const std::string* current = value ? std::get_if<std::string>(&*value) : nullptr;
                text = current ? *current : std::string{};
            }
            const ListenServerRule* target = &rule;
            const FormTextSpec spec{.max_len = 63, .filter = ui_char_filter_printable, .hint = ""};
            form_text(ctx, line, id, text, spec, fonts.body, g_state.prev_text_focus, [scope, target, &text] {
                g_state.setup.set_rule(scope, *target, text);
            });
            break;
        }
    }
}

// ─── Saved setups ───

void refresh_saved_setups(std::string_view select)
{
    g_saved_setups = listen_server_saved_setups();
    g_saved_index = -1;
    for (size_t i = 0; i < g_saved_setups.size(); ++i) {
        if (string_iequals(g_saved_setups[i], select)) {
            g_saved_index = static_cast<int>(i);
        }
    }
}

void report_warnings(const char* title, const std::vector<std::string>& warnings)
{
    if (warnings.empty()) {
        return;
    }
    std::string text;
    for (size_t i = 0; i < warnings.size(); ++i) {
        rf::console::print("{}\n", warnings[i]);
        if (i < max_warnings_shown) {
            text += (text.empty() ? "" : " ") + warnings[i];
        }
    }
    if (warnings.size() > max_warnings_shown) {
        text += std::format(" {} more in the console.", warnings.size() - max_warnings_shown);
    }
    create_game_notice(title, std::move(text));
}

void install_setup(ListenServerSetup&& loaded)
{
    // The cells some box may still be writing into go below.
    g_menu.ui().release_text_focus(false);
    drop_cells();
    g_state.setup = std::move(loaded);
    g_password_text = g_state.setup.password();
    const rf::NetGameType game_type = g_state.game_type;
    if (g_state.setup.num_levels() > 0) {
        g_state.game_type = g_state.setup.game_type(0);
    }
    g_maps.reset();
    if (g_state.game_type != game_type) {
        g_maps.follow_game_type(g_state.game_type);
    }
    g_bots.reset();
}

void save_setup(const std::string& name)
{
    if (!listen_server_save_setup(create_game_setup(), name)) {
        create_game_notice("Save Setup", std::format("{} could not be saved.", name));
        return;
    }
    g_setup_name = name;
    refresh_saved_setups(name);
}

void ask_save_name()
{
    g_menu.ui().release_text_focus(true);
    create_game_flush_edits();
    const std::string initial = g_saved_index >= 0 ? g_saved_setups[g_saved_index] : std::string{};
    g_menu.popup().open_input("Save Setup", "A name for this setup:", initial, 64, listen_server_setup_name_char,
                              "SAVE",
        [](int button, const std::string& text) {
            if (button != 1) {
                return;
            }
            const std::string name{trim(text)};
            if (!listen_server_setup_name_valid(name)) {
                create_game_notice("Save Setup",
                                   "A name takes letters, digits, spaces, - and _, and cannot be a device name such as "
                                   "CON or NUL.");
                return;
            }
            const auto there = std::find_if(g_saved_setups.begin(), g_saved_setups.end(),
                [&](const std::string& saved) { return string_iequals(saved, name); });
            if (there == g_saved_setups.end()) {
                save_setup(name);
                return;
            }
            const std::string existing = *there;
            g_menu.popup().open("Save Setup", std::format("Replace the saved setup {}?", existing),
                                {{"CANCEL"}, {"REPLACE", rf::KEY_R}}, 0, 0,
                                [existing](int pressed, const std::string&) {
                                    if (pressed == 1) {
                                        save_setup(existing);
                                    }
                                });
        });
}

void load_saved_setup(const std::string& name)
{
    std::vector<std::string> warnings;
    std::optional<ListenServerSetup> loaded = listen_server_load_setup(name, warnings);
    if (!loaded) {
        create_game_notice("Load Setup", warnings.empty() ? std::format("{} could not be read.", name)
                                                          : warnings.front());
        return;
    }
    install_setup(std::move(*loaded));
    g_setup_name = name;
    report_warnings("Load Setup", warnings);
}

// ─── The column ───

std::vector<ColumnButton> make_buttons()
{
    return {
        ColumnButton{action_id(Action::Options), menu_stock_label(rf::strings::options), rf::KEY_O, true},
        ColumnButton{action_id(Action::Maps), "MAPS", rf::KEY_M, true},
        ColumnButton{action_id(Action::Bots), "BOTS", rf::KEY_T, true},
        ColumnButton{action_id(Action::Mutators), "MUTATORS", rf::KEY_U, true},
        ColumnButton{action_id(Action::Start), menu_stock_label(rf::strings::start), rf::KEY_S, false},
    };
}

void start()
{
    g_menu.ui().release_text_focus(true);
    create_game_flush_edits();
    if (create_game_setup().num_levels() == 0) {
        create_game_notice("No Maps", "Add maps to the rotation on the MAPS panel first.");
        g_menu.open_panel(&g_maps);
        return;
    }
    // The next launch's default setup starts from these, as LAN only does from its toggle.
    g_alpine_game_config.create_game_server_name = create_game_setup().server_name();
    g_alpine_game_config.create_game_password = create_game_setup().password();
    // Written now, so a crash or kill during the session does not lose them.
    alpine_core_config_save();
    map_thumbnails_release_bitmaps();
    switch (start_listen_server(create_game_setup())) {
        case ListenServerStartResult::NoLevels:
            create_game_notice("No Maps", "None of the rotation's maps is installed.");
            break;
        case ListenServerStartResult::StartedLanOnly:
            // Stock's own message, over the level that loads.
            rf::ui::popup_message(rf::strings::error, rf::strings::tracker_init_failed, nullptr, false);
            break;
        case ListenServerStartResult::Started:
            break;
    }
}

void run_action(Action action)
{
    switch (action) {
        case Action::Options:
            g_menu.open_panel(&g_options);
            break;
        case Action::Maps:
            g_menu.open_panel(&g_maps);
            break;
        case Action::Bots:
            g_menu.open_panel(&g_bots);
            break;
        case Action::Mutators:
            g_menu.open_panel(&g_mutators);
            break;
        case Action::Start:
            start();
            break;
        case Action::Back:
            if (!g_menu.closing()) {
                g_menu.ui().release_text_focus(true);
                create_game_flush_edits();
                g_menu.request_close();
                rf::gameseq_pop_state();
            }
            break;
    }
    if (!g_menu.jump_action()) {
        ui_play_click();
    }
}

void __cdecl key_callback(int key)
{
    g_menu.key(key);
}

std::vector<CreateGameMap> list_maps()
{
    std::vector<CreateGameMap> maps;
    for (std::string& filename : listen_server_levels()) {
        const std::string key = string_to_lower(filename);
        auto it = g_level_names.find(key);
        if (it == g_level_names.end()) {
            rf::String name;
            const bool read = rf::ui::create_game_get_level_name(rf::String{filename.c_str()}, name);
            it = g_level_names.emplace(key, read && name.c_str()[0] ? std::string{name.c_str()} : filename).first;
        }
        maps.push_back(CreateGameMap{std::move(filename), it->second});
    }
    return maps;
}

// ─── OPTIONS rows ───

// The rules of this group are rows of OPTIONS' own Server group.
constexpr std::string_view server_group = "Server";

bool options_server_rule(const ListenServerRule& rule)
{
    return rule.group == server_group && rule_listed(ListenServerSetup::base_scope, rule);
}

bool options_rule_shown(const ListenServerRule& rule, int)
{
    return rule.group != server_group && create_game_rule_in_play(rule);
}

const std::vector<std::string>& game_type_names()
{
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        for (int type = 0; type < rf::NG_TYPE_UNK; ++type) {
            out.emplace_back(multi_game_type_name(static_cast<rf::NetGameType>(type)));
        }
        return out;
    }();
    return names;
}

constexpr int options_rows = 7;

} // namespace

// ─── Shared ───

CreateGameState& create_game()
{
    return g_state;
}

void create_game_notice(std::string title, std::string text)
{
    g_menu.popup().open(std::move(title), std::move(text), {{"OK"}}, 0, 0, nullptr, MenuPopupStyle::Neutral);
}

bool create_game_popup_active()
{
    return g_menu.popup().active();
}

std::string create_game_map_name(std::string_view filename)
{
    const auto it = std::find_if(g_state.maps.begin(), g_state.maps.end(),
        [&](const CreateGameMap& map) { return string_iequals(map.filename, filename); });
    return it != g_state.maps.end() ? it->name : std::string{filename};
}

std::vector<rf::NetGameType> create_game_types_in_play()
{
    std::vector<rf::NetGameType> types;
    for (int i = 0; i < create_game_setup().num_levels(); ++i) {
        const rf::NetGameType type = create_game_setup().game_type(i);
        if (std::find(types.begin(), types.end(), type) == types.end()) {
            types.push_back(type);
        }
    }
    if (types.empty()) {
        types.push_back(g_state.game_type);
    }
    return types;
}

bool create_game_any_type_in(uint32_t game_type_mask)
{
    const std::vector<rf::NetGameType> types = create_game_types_in_play();
    return std::any_of(types.begin(), types.end(), [&](rf::NetGameType type) {
        return mutator_gametype_mask_allows(game_type_mask, static_cast<uint8_t>(type));
    });
}

bool create_game_bots_in_play()
{
    return create_game_any_type_in(mutator_gametype_mask_for_req(MutatorGametypeReq::BotsSupported));
}

bool create_game_teams_in_play()
{
    const std::vector<rf::NetGameType> types = create_game_types_in_play();
    return std::any_of(types.begin(), types.end(), [](rf::NetGameType type) {
        return multi_game_type_is_team_type(type);
    });
}

int create_game_bots_limit()
{
    return std::clamp(create_game_setup().max_players() - 1, 0, k_max_server_bots);
}

bool create_game_rule_in_play(const ListenServerRule& rule)
{
    return create_game_any_type_in(rule.game_type_mask);
}

int create_game_rules_height(const UiContext& ctx, int scope, CreateGameRuleFilter shown, bool basic_groups_first)
{
    int h = 0;
    for_each_shown_rule(scope, shown, basic_groups_first, [&](const ListenServerRule&, bool heading) {
        h += skin_px(ctx, skin::row_h) + (heading ? ctx.scaled(form_sub_header_h) : 0);
    });
    return h;
}

void create_game_rule_rows(UiContext& ctx, FormScroll& scroll, int x, int& y, int scope, CreateGameRuleFilter shown,
                           const FormFonts& fonts, bool basic_groups_first)
{
    for_each_shown_rule(scope, shown, basic_groups_first, [&](const ListenServerRule& rule, bool heading) {
        if (heading) {
            if (const auto rect =
                    form_scroll_row(ctx, scroll, x, y, ctx.scaled(form_sub_header_h), 0, UiFocusKind::Activate)) {
                form_draw_sub_header(ctx, *rect, rule.group, fonts.body_bold);
            }
        }
        build_rule_row(ctx, scroll, x, y, scope, rule, fonts);
    });
}

int create_game_mutators_height(const UiContext& ctx, int scope)
{
    int h = 0;
    const std::vector<MutatorInfo>& registry = mutators_get_registry();
    for (const MutatorInfo& info : registry) {
        if (!mutator_listed(info, scope)) {
            continue;
        }
        std::vector<MutatorEditorOption> options;
        for (const MutatorOptionInfo& option : info.options) {
            options.push_back(MutatorEditorOption{option.label.c_str(), option.type, nullptr, false});
        }
        const MutatorEditor editor{
            .enabled = scope != ListenServerSetup::base_scope || create_game_any_type_in(info.valid_gametype_mask),
            .options = options,
        };
        h += mutator_editor_height(ctx, editor, mutator_on(scope, info));
    }
    return h;
}

void create_game_mutator_rows(UiContext& ctx, FormScroll& scroll, int x, int& y, int scope, const FormFonts& fonts)
{
    const std::vector<MutatorInfo>& registry = mutators_get_registry();
    for (size_t i = 0; i < registry.size(); ++i) {
        const MutatorInfo& info = registry[i];
        if (!mutator_listed(info, scope)) {
            continue;
        }
        const int key = mutator_key(scope, i);
        MutatorCell& cell = mutator_cell(key, scope, info);
        const std::vector<MutatorEditorOption> options = mutator_options(cell, info, scope);
        const MutatorEditor editor{
            .key = key,
            .label = info.label.c_str(),
            .desc_id = desc_mutators + static_cast<int>(i),
            .enabled = scope != ListenServerSetup::base_scope || create_game_any_type_in(info.valid_gametype_mask),
            .modified = create_game_setup().own_mutator(scope, info.name).has_value(),
            .options = options,
        };
        const MutatorEditorResult result =
            mutator_editor_rows(ctx, scroll, x, y, editor, cell.on, cell.values, fonts, g_state.prev_text_focus);
        if (ctx.draw) {
            continue;
        }
        if (result.reset) {
            mark_clean(cell);
            create_game_setup().set_mutator(scope, info.name, std::nullopt);
            ui_play_click();
        }
        else if (result.reset_option >= 0) {
            mark_clean(cell);
            create_game_setup().unset_mutator_option(scope, info.name, info.options[result.reset_option].name);
            ui_play_click();
        }
        else if (result.changed) {
            flush_mutator(cell);
        }
    }
}

void create_game_flush_edits()
{
    for (auto& [id, cell] : g_numbers) {
        flush_number(cell);
    }
    for (auto& [key, cell] : g_mutator_cells) {
        flush_mutator(cell);
    }
}

void create_game_draw_description(UiContext& ctx, const UiRect& strip, int font)
{
    const int id = ctx.description().id;
    const auto rules = listen_server_rules();
    std::string text;
    const std::vector<MutatorInfo>& registry = mutators_get_registry();
    if (id >= 0 && id < static_cast<int>(rules.size())) {
        text = rules[id].description;
    }
    else if (id >= desc_mutators && id < desc_mutators + static_cast<int>(registry.size())) {
        text = mutator_description(registry[id - desc_mutators].name);
    }
    for (const auto& [desc, custom] : custom_descriptions) {
        if (desc == id) {
            text = custom;
        }
    }
    form_draw_description(ctx, strip, text, font);
}

bool create_game_edit_button(UiContext& ctx, const UiRect& r, bool own, bool flash, UiId id)
{
    if (flash || own) {
        const rf::Color held = flash ? skin::txt_hi : skin::gold;
        return skin_icon_button(ctx, r, "af_edit_icon", held, held, id);
    }
    return skin_icon_button(ctx, r, "af_edit_icon", skin::txt_hint, skin::txt_hi, id);
}

UiRect create_game_dialog(const UiContext& ctx, int w_ref, int h_ref, const char* title, UiRect& dialog)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const MenuDialog frame = menu_dialog(ctx, s(static_cast<float>(w_ref)), s(static_cast<float>(h_ref)), title,
                                         menu_font(true, 26.0f, menu_screen_scale()));
    dialog = frame.frame;
    const int pad = s(24.0f);
    const int top = frame.band.y + frame.band.h + s(12.0f);
    return UiRect{dialog.x + pad, top, dialog.w - 2 * pad, dialog.y + dialog.h - pad - top};
}

int create_game_dialog_note(const UiContext& ctx, const UiRect& area, const std::string& note, int font)
{
    const int line_h = rf::gr::get_font_height(font) + skin_px(ctx, 4.0f);
    const std::vector<std::string> lines{note};
    const int h = ui_line_block_height(lines, area.w, font, line_h, 2 * line_h);
    if (ctx.draw) {
        ui_draw_line_block(UiRect{area.x, area.y, area.w, h}, lines, font, line_h, skin::txt_hint.red,
                           skin::txt_hint.green, skin::txt_hint.blue);
    }
    return h + skin_px(ctx, 4.0f);
}

CreateGamePanes create_game_panes(const UiContext& ctx, const MenuPanel& panel, const char* heading,
                                  const std::string& count, const MenuFonts& fonts)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const FormPanelAreas areas = form_panel_areas(ctx, panel, false);
    const UiRect& body = areas.body;
    const int gap = s(24.0f);
    const int left_w = (body.w - gap) / 2;
    const int right_x = body.x + left_w + gap;
    const int right_w = body.x + body.w - right_x;
    const int bar_h = s(skin::widget_h);
    const int well_y = body.y + bar_h + s(12.0f);
    const int well_h = std::max(s(40.0f), body.y + body.h - well_y);
    if (ctx.draw) {
        const int ty = body.y + (bar_h - rf::gr::get_font_height(fonts.column)) / 2;
        skin_draw_text(right_x + s(4.0f), ty, heading, fonts.column, skin::ink_title);
        const int count_w = right_w - s(8.0f) - rf::gr::get_string_size(heading, fonts.column).first - s(16.0f);
        rf::gr::set_color(skin::ink_hint);
        rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, right_x + right_w - s(4.0f), ty,
                               ui_fit_end(count, std::max(0, count_w), fonts.body).c_str(), fonts.body);
    }
    const UiRect& footer = areas.footer;
    return CreateGamePanes{
        .bar = UiRect{body.x, body.y, left_w, bar_h},
        .left = UiRect{body.x, well_y, left_w, well_h},
        .right = UiRect{right_x, well_y, right_w, well_h},
        .left_footer = UiRect{body.x, footer.y, left_w, footer.h},
        .right_footer = UiRect{right_x, footer.y, right_w, footer.h},
    };
}

CreateGameEntry create_game_entry(const UiContext& ctx, const SkinScrollListRow& row, int cycler_w)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const UiRect& r = row.rect;
    const int widget_h = s(skin::widget_h);
    CreateGameEntry entry;
    entry.cycler = UiRect{r.x + r.w - s(8.0f) - cycler_w, r.y + (r.h - widget_h) / 2, cycler_w, widget_h};
    const int edit_r = cycler_w > 0 ? entry.cycler.x - s(8.0f) : entry.cycler.x;
    entry.edit = UiRect{edit_r - widget_h, entry.cycler.y, widget_h, widget_h};
    entry.hovered = ui_hover(ctx, r, row.id);
    entry.on_cycler = ui_hover(ctx, entry.cycler);
    entry.on_edit = ui_hover(ctx, entry.edit);
    return entry;
}

CreateGameEntryClick create_game_entry_click(const UiContext& ctx, const SkinListRow& r, const CreateGameEntry& entry)
{
    if (r.activated || (r.clicked && entry.on_row())) {
        return CreateGameEntryClick::Pick;
    }
    if (ctx.mclick && entry.on_row()) {
        return CreateGameEntryClick::Remove;
    }
    if (ctx.rclick && entry.hovered && !entry.on_cycler) {
        return CreateGameEntryClick::Edit;
    }
    return CreateGameEntryClick::None;
}

int create_game_draw_entry_index(const UiContext& ctx, const UiRect& d, int index, int font)
{
    const int index_w = rf::gr::get_string_size("00", font).first + skin_px(ctx, 16.0f);
    rf::gr::set_color(skin::txt_hint);
    rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, d.x + index_w, d.y + (d.h - rf::gr::get_font_height(font)) / 2,
                           std::to_string(index + 1).c_str(), font);
    return d.x + skin_px(ctx, 8.0f) + index_w;
}

void create_game_reveal(const UiContext& ctx, const UiRect& well, const UiRect& row, float& scroll)
{
    const int top = well.y + skin_px(ctx, 4.0f);
    const int bottom = well.y + well.h - skin_px(ctx, 4.0f);
    if (row.y < top) {
        scroll -= static_cast<float>(top - row.y);
    }
    else if (row.y + row.h > bottom) {
        scroll += static_cast<float>(row.y + row.h - bottom);
    }
}

CreateGameListOp create_game_list_buttons(UiContext& ctx, const UiRect& footer, int selected, int count, bool movable,
                                          int font, const char* id_name)
{
    struct Button
    {
        const char* label;
        bool enabled;
        CreateGameListOp op;
    };
    const bool has_sel = movable && selected >= 0 && selected < count;
    const std::array<Button, 4> buttons{{
        {"Up", has_sel && selected > 0, CreateGameListOp::Up},
        {"Down", has_sel && selected < count - 1, CreateGameListOp::Down},
        {"Remove", has_sel, CreateGameListOp::Remove},
        {"Clear", count > 0, CreateGameListOp::Clear},
    }};
    const int n = static_cast<int>(buttons.size());
    const int gap = skin_px(ctx, 10.0f);
    const int bw = (footer.w - (n - 1) * gap) / n;
    CreateGameListOp pressed = CreateGameListOp::None;
    for (int i = 0; i < n; ++i) {
        const int x = footer.x + i * (bw + gap);
        const UiRect r{x, footer.y, i == n - 1 ? footer.x + footer.w - x : bw, footer.h};
        if (skin_button(ctx, r, buttons[i].label, font, SkinButton::Secondary, buttons[i].enabled, ui_id(id_name, i))) {
            pressed = buttons[i].op;
        }
    }
    if (pressed != CreateGameListOp::None) {
        ui_play_click();
    }
    return pressed;
}

// ─── OPTIONS ───

void CreateGameRowsPanel::reset()
{
    open_ = false;
    scroll_ = 0.0f;
    scrolled_to_ = 0;
}

int CreateGameRowsPanel::options_height(const UiContext& ctx) const
{
    const int rows = options_rows + static_cast<int>(std::ranges::count_if(listen_server_rules(), options_server_rule));
    return rows * skin_px(ctx, skin::row_h) + ctx.scaled(form_sub_header_h);
}

void CreateGameRowsPanel::build_options(UiContext& ctx, FormScroll& scroll, int x, int& y, const FormFonts& ff)
{
    ListenServerSetup& s = create_game_setup();
    const int row_h = skin_px(ctx, skin::row_h);
    const UiId prev_focus = g_state.prev_text_focus;
    const auto line_for = [&](const UiRect& rect, UiId id, const char* label, int desc, bool enabled = true) {
        return form_row(ctx, FormRow{.rect = rect, .id = id, .label = label, .enabled = enabled, .desc_id = desc}, ff);
    };

    {
        const UiId drop_id = ui_id("cg_setup");
        const UiId save_id = ui_id("cg_setup_save");
        const UiId load_id = ui_id("cg_setup_load");
        const bool any = !g_saved_setups.empty();
        if (const auto rect = form_scroll_row(ctx, scroll, x, y, row_h, any ? drop_id : 0, UiFocusKind::Activate)) {
            const FormLine line = line_for(*rect, drop_id, "Saved setup", desc_setup);
            const int gap = ctx.scaled(10.0f);
            const int bw = std::max(skin_button_width(ctx, "SAVE", ff.body, 96.0f),
                                    skin_button_width(ctx, "LOAD", ff.body, 96.0f));
            const int h = skin_px(ctx, skin::widget_h);
            const int by = rect->y + (rect->h - h) / 2;
            const UiRect load{line.ctl_right - bw, by, bw, h};
            const UiRect save{load.x - gap - bw, by, bw, h};
            const int dw = skin_px(ctx, skin::control_w);
            const UiRect drop{save.x - gap - dw, by, dw, h};
            static const std::vector<std::string> none{"None saved"};
            int sel = any ? g_saved_index : 0;
            skin_dropdown(ctx, drop, any ? g_saved_setups : none, sel, ff.body, any, drop_id, line.hot);
            if (any) {
                g_saved_index = std::clamp(sel, -1, static_cast<int>(g_saved_setups.size()) - 1);
            }
            if (skin_button(ctx, save, "SAVE", ff.body, SkinButton::Secondary, true, save_id)) {
                ui_play_click();
                ask_save_name();
            }
            const bool picked = g_saved_index >= 0;
            if (skin_button(ctx, load, "LOAD", ff.body, SkinButton::Secondary, picked, load_id) && picked) {
                ui_play_click();
                load_saved_setup(g_saved_setups[g_saved_index]);
            }
        }
        else {
            ctx.register_focusable(save_id, UiFocusKind::Activate);
            if (g_saved_index >= 0) {
                ctx.register_focusable(load_id, UiFocusKind::Activate);
            }
        }
    }

    if (const auto rect = form_scroll_row(ctx, scroll, x, y, ctx.scaled(form_sub_header_h), 0, UiFocusKind::Activate)) {
        form_draw_sub_header(ctx, *rect, "Server", ff.body_bold);
    }

    {
        const UiId id = ui_id("cg_server_name");
        if (!ctx.draw && ctx.focused_text_id() != id) {
            g_name_text = s.server_name();
        }
        if (const auto rect = form_scroll_row(ctx, scroll, x, y, row_h, id, UiFocusKind::Text)) {
            const FormLine line = line_for(*rect, id, rf::strings::server_name, desc_server_name);
            const FormTextSpec spec{.max_len = 30, .filter = ui_char_filter_printable, .hint = ""};
            form_text(ctx, line, id, g_name_text, spec, ff.body, prev_focus,
                      [] { create_game_setup().set_server_name(g_name_text); });
        }
    }

    {
        const UiId id = ui_id("cg_password");
        if (!ctx.draw && ctx.focused_text_id() != id) {
            g_password_text = s.password();
        }
        if (const auto rect = form_scroll_row(ctx, scroll, x, y, row_h, id, UiFocusKind::Text)) {
            const FormLine line = line_for(*rect, id, "Server password", desc_password_text);
            // Plain text, as the browser's password prompt.
            const FormTextSpec spec{.max_len = 16, .filter = ui_char_filter_printable, .hint = ""};
            form_text(ctx, line, id, g_password_text, spec, ff.body, prev_focus,
                      [] { create_game_setup().set_password(g_password_text); });
        }
    }

    {
        const UiId id = ui_id("cg_max_players");
        if (const auto rect = form_scroll_row(ctx, scroll, x, y, row_h, id, UiFocusKind::Adjustable)) {
            const FormLine line = line_for(*rect, id, rf::strings::max_players, desc_max_players);
            number_row(ctx, line, id, static_cast<float>(s.max_players()), 1.0f, 32.0f, 1.0f, UiNumberFormat{}, true,
                       ff.body, [](float v) { create_game_setup().set_max_players(static_cast<int>(std::lround(v))); });
        }
    }

    {
        const UiId id = ui_id("cg_lan_only");
        if (const auto rect = form_scroll_row(ctx, scroll, x, y, row_h, id, UiFocusKind::Adjustable)) {
            const FormLine line = line_for(*rect, id, rf::strings::lan_only, desc_lan_only);
            if (form_toggle(ctx, line, id, g_alpine_game_config.create_game_lan_only, ff.body)) {
                alpine_core_config_save();
            }
        }
    }

    for (const ListenServerRule& rule : listen_server_rules()) {
        if (options_server_rule(rule)) {
            build_rule_row(ctx, scroll, x, y, ListenServerSetup::base_scope, rule, ff);
        }
    }

    {
        const UiId id = ui_id("cg_game_type");
        if (const auto rect = form_scroll_row(ctx, scroll, x, y, row_h, id, UiFocusKind::Activate)) {
            const FormLine line = line_for(*rect, id, rf::strings::game_type, desc_game_type);
            int type = g_state.game_type;
            if (form_choice(ctx, line, id, game_type_names(), type, ff.body)) {
                g_state.game_type = static_cast<rf::NetGameType>(type);
                g_maps.follow_game_type(g_state.game_type);
            }
        }
    }

    {
        const UiId id = ui_id("cg_bots");
        const bool allowed = create_game_bots_in_play();
        if (const auto rect =
                form_scroll_row(ctx, scroll, x, y, row_h, allowed ? id : 0, UiFocusKind::Adjustable)) {
            const FormLine line = line_for(*rect, id, "Bots", desc_bots, allowed);
            const int count = static_cast<int>(s.bots().size());
            const float limit = static_cast<float>(std::max(create_game_bots_limit(), count));
            number_row(ctx, line, id, static_cast<float>(count), 0.0f, limit, 1.0f, UiNumberFormat{}, true, ff.body,
                       [](float v) { create_game_set_bot_count(static_cast<int>(std::lround(v))); });
        }
    }
}

void CreateGameRowsPanel::build(UiContext& ctx, const MenuPanel& panel, const MenuFonts& fonts)
{
    const FormFonts ff = form_fonts(fonts);
    if (ctx.draw) {
        const std::string heading = menu_stock_label(rf::strings::options);
        menu_draw_panel_heading(ctx, panel, heading.c_str(), fonts.heading);
    }
    const FormPanelAreas areas = form_panel_areas(ctx, panel, true, false);
    const UiRect view = form_rows_view(ctx, areas.body);
    const int row_h = skin_px(ctx, skin::row_h);
    const int scope = ListenServerSetup::base_scope;
    const int content_h = options_height(ctx) + create_game_rules_height(ctx, scope, options_rule_shown, true);
    FormScroll scroll = form_scroll_begin(ctx, view, content_h, scroll_, row_h * 3, ui_id("cg_rows"));
    int y = scroll.region.start_y();
    build_options(ctx, scroll, view.x, y, ff);
    // Groups with a basic rule lead, its basic rows first.
    create_game_rule_rows(ctx, scroll, view.x, y, scope, options_rule_shown, ff, true);
    form_scroll_end(ctx, scroll, scrolled_to_);
    if (ctx.draw) {
        create_game_draw_description(ctx, areas.desc, fonts.caption);
    }
}

void CreateGameRowsPanel::commit()
{
    create_game_flush_edits();
}

// ─── MUTATORS ───

void CreateGameMutatorsPanel::reset()
{
    open_ = false;
    scroll_ = 0.0f;
    scrolled_to_ = 0;
}

void CreateGameMutatorsPanel::build(UiContext& ctx, const MenuPanel& panel, const MenuFonts& fonts)
{
    const FormFonts ff = form_fonts(fonts);
    if (ctx.draw) {
        menu_draw_panel_heading(ctx, panel, "MUTATORS", fonts.heading);
    }
    const FormPanelAreas areas = form_panel_areas(ctx, panel, true, false);
    const UiRect view = form_rows_view(ctx, areas.body);
    const int scope = ListenServerSetup::base_scope;
    FormScroll scroll = form_scroll_begin(ctx, view, create_game_mutators_height(ctx, scope), scroll_,
                                          skin_px(ctx, skin::row_h) * 3, ui_id("cg_mutator_rows"));
    int y = scroll.region.start_y();
    create_game_mutator_rows(ctx, scroll, view.x, y, scope, ff);
    form_scroll_end(ctx, scroll, scrolled_to_);
    if (ctx.draw) {
        create_game_draw_description(ctx, areas.desc, fonts.caption);
    }
}

void CreateGameMutatorsPanel::commit()
{
    create_game_flush_edits();
}

// ─── The screen ───

void create_game_menu_enter()
{
    drop_cells();
    if (!std::exchange(g_loaded, true)) {
        install_setup(listen_server_default_setup());
    }
    g_state.maps = list_maps();
    g_state.prev_text_focus = 0;
    g_rule_choices.clear();
    refresh_saved_setups(g_setup_name);

    const int back = action_id(Action::Back);
    g_menu.enter("HOST GAME", make_buttons(), ColumnButton{back, menu_stock_label(rf::strings::back), rf::KEY_B}, back);
    g_options.reset();
    g_maps.reset();
    g_bots.reset();
    g_mutators.reset();
    g_menu.add_panel(&g_options, action_id(Action::Options));
    g_menu.add_panel(&g_maps, action_id(Action::Maps));
    g_menu.add_panel(&g_bots, action_id(Action::Bots));
    g_menu.add_panel(&g_mutators, action_id(Action::Mutators));
    // As stock's, the screen opens on OPTIONS.
    g_menu.seat_panel(&g_options);
}

void create_game_menu_do_frame(bool paused)
{
    // A box the draw pass let go of committed after the last flush.
    create_game_flush_edits();
    g_menu.step();
    if (!paused) {
        const std::optional<int> action = g_menu.input(key_callback);
        g_state.prev_text_focus = g_menu.ui().focused_text_id();
        if (action) {
            run_action(static_cast<Action>(*action));
        }
    }
    g_menu.draw();
}

bool create_game_menu_is_closed()
{
    // Changes the menu did not start (a hosted game loading) do not wait for the slide.
    return !g_menu.closing() || g_menu.is_closed();
}

void create_game_menu_on_close(bool immediately)
{
    g_menu.ui().release_text_focus(false);
    g_maps.close();
    g_bots.close();
    map_thumbnails_release_bitmaps();
    if (!immediately) {
        rf::snd_play(stock_sound_id::menu_slide_out, 0, 0.0f, 1.0f);
    }
}
