#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include "mutator_editors.h"
#include "settings_skin.h"

namespace
{

constexpr float option_indent = 28.0f;

struct MutatorDescription
{
    const char* name;
    const char* text;
};

// Keyed by the mutator's registry name, which the vote-options blob also carries.
const MutatorDescription mutator_descriptions[] = {
    {"instagib",
     "Spawn with Rail. One-hit kills, unlimited ammo, no reloads, no weapon switching, no pickups."},
    {"oneweapon",
     "Spawn with Baton. Weapon/ammo pickups based on the specified weapon. No other pickups."},
    {"arena",
     "Spawn with AR and 100/100. Auto reload and reset to 100/100 on a frag. Weapon pickups only."},
    {"vampire",
     "Damage enemies to regenerate health and armor."},
    {"crits",
     "Random critical hits, ramps with recent damage dealt."},
    {"superdrain",
     "Super health and armor drain down to 100 over time."},
    {"armored",
     "Players spawn with 100 armor."},
    {"superrail",
     "Rail gun pickups are treated as super items."},
    {"bigcraters",
     "Geomod craters are much larger."},
    {"flamingenemies",
     "Continuous flamethrower damage sets enemies on fire."},
    {"gibbing",
     "Explosive deaths chunk enemies into gibs."},
    {"jetpacks",
     "All players get jetpacks."},
    {"humansvsbots",
     "Bots play on the Blue team. Humans play on the Red team."},
    {"delayedsupers",
     "Super items start the round hidden, then respawn normally."},
    {"weirdgungame",
     "Gun Game includes weird creature weapons and lasers."},
    {"lowgravity",
     "Low gravity for all players and projectiles."},
    {"skiing",
     "Hold crouch to ski - maintain speed and build downhill."},
    {"dodging",
     "Double-tap or crouch-jump a direction to dodge."},
    {"pogo",
     "Hold jump to hop automatically when you land."},
    {"scorelimit",
     "Overrides the score limit of the game type being played."},
    {"idealplayers",
     "Sets the player count the server advertises as its target size."},
    {"joinbots",
     "Bots join for the level, or fill a match's open slots once the other players are ready."},
    {"itemreplace",
     "Swaps super pickups for other powerups, or removes them. Pick one or more replacement sets."},
};

int option_key(const MutatorEditor& mutator, std::size_t index)
{
    return mutator.key * 256 + static_cast<int>(index);
}

UiFocusKind option_focus_kind(const MutatorEditorOption& option)
{
    switch (option.type) {
        case MutatorOptionType::Int:
        case MutatorOptionType::Float:
            return UiFocusKind::Text;
        case MutatorOptionType::Choice:
            return option.choices && static_cast<int>(option.choices->size()) > form_cycler_max_options
                ? UiFocusKind::Activate
                : UiFocusKind::Adjustable;
        default:
            return UiFocusKind::Adjustable;
    }
}

void parse_number(const std::string& text, MutatorOptionType type, MutatorEditorValue& value)
{
    try {
        if (type == MutatorOptionType::Int) {
            value.int_value = std::stoi(text);
            value.player_set = true;
        }
        else if (const float parsed = std::stof(text); std::isfinite(parsed)) {
            value.float_value = parsed;
            value.player_set = true;
        }
    }
    catch (const std::exception&) {
        // Empty or out of range: the value stays.
    }
}

// True when the player changed the value.
bool build_option(UiContext& ctx, const UiRect& rect, const MutatorEditor& mutator, std::size_t index,
                  MutatorEditorValue& value, const FormFonts& fonts, UiId prev_text_focus, MutatorEditorResult& result)
{
    const MutatorEditorOption& option = mutator.options[index];
    const UiId id = ui_id("mutator_option", option_key(mutator, index));
    const FormRow row{
        .rect = rect,
        .id = id,
        .label = option.label,
        .modified = option.modified,
        .reset_id = ui_id("mutator_option_reset", option_key(mutator, index)),
        .desc_id = mutator.desc_id,
    };
    const FormLine line = form_row(ctx, row, fonts);
    if (line.reset) {
        result.reset_option = static_cast<int>(index);
    }

    switch (option.type) {
        case MutatorOptionType::Bool:
            return form_toggle(ctx, line, id, value.bool_value, fonts.body);
        case MutatorOptionType::Choice: {
            const int count = option.choices ? static_cast<int>(option.choices->size()) : 0;
            if (count == 0) {
                form_value_text(ctx, line, "-", fonts.body);
                return false;
            }
            int choice = std::min(static_cast<int>(value.choice_index), count - 1);
            if (!form_choice(ctx, line, id, *option.choices, choice, fonts.body)) {
                return false;
            }
            value.choice_index = static_cast<uint8_t>(choice);
            return true;
        }
        case MutatorOptionType::Int:
        case MutatorOptionType::Float: {
            const bool integral = option.type == MutatorOptionType::Int;
            const std::string shown =
                integral ? std::format("{}", value.int_value) : std::format("{:.2f}", value.float_value);
            // Without trailing zeros: "30", not "30.00".
            const std::string seed =
                integral ? std::format("{}", value.int_value) : std::format("{:g}", value.float_value);
            MutatorEditorValue* target = &value;
            const MutatorOptionType type = option.type;
            const FormTextSpec spec{.max_len = 15, .filter = ui_char_filter_numeric, .hint = ""};
            // Taken on the focus rather than the commit: the box owns the number from here.
            return form_number_text(ctx, line, id, shown, seed, spec, fonts.body, prev_text_focus,
                                    [target, type](const std::string& text) { parse_number(text, type, *target); });
        }
        default:
            return false;
    }
}

// Types with no editor (String) take no row.
bool mutator_option_has_editor(MutatorOptionType type)
{
    switch (type) {
        case MutatorOptionType::Bool:
        case MutatorOptionType::Choice:
        case MutatorOptionType::Int:
        case MutatorOptionType::Float:
            return true;
        default:
            return false;
    }
}

} // namespace

int mutator_editor_height(const UiContext& ctx, const MutatorEditor& mutator, bool on)
{
    const int row_h = skin_px(ctx, skin::row_h);
    int h = row_h;
    if (on && mutator.enabled) {
        for (const MutatorEditorOption& option : mutator.options) {
            if (mutator_option_has_editor(option.type)) {
                h += row_h;
            }
        }
    }
    return h;
}

MutatorEditorResult mutator_editor_rows(UiContext& ctx, FormScroll& scroll, int x, int& y, const MutatorEditor& mutator,
                                        bool& on, std::span<MutatorEditorValue> values, const FormFonts& fonts,
                                        UiId prev_text_focus)
{
    MutatorEditorResult result;
    const int row_h = skin_px(ctx, skin::row_h);
    const UiId id = ui_id("mutator", mutator.key);
    if (const auto rect =
            form_scroll_row(ctx, scroll, x, y, row_h, mutator.enabled ? id : 0, UiFocusKind::Adjustable)) {
        const FormRow row{
            .rect = *rect,
            .id = id,
            .label = mutator.label,
            .enabled = mutator.enabled,
            .modified = mutator.modified,
            .reset_id = ui_id("mutator_reset", mutator.key),
            .desc_id = mutator.desc_id,
        };
        const FormLine line = form_row(ctx, row, fonts);
        result.reset = line.reset;
        result.changed = form_toggle(ctx, line, id, on, fonts.body);
    }
    if (!on || !mutator.enabled) {
        return result;
    }

    const int indent = skin_px(ctx, option_indent);
    const std::size_t count = std::min(mutator.options.size(), values.size());
    for (std::size_t i = 0; i < count; ++i) {
        const MutatorEditorOption& option = mutator.options[i];
        if (!mutator_option_has_editor(option.type)) {
            continue;
        }
        const UiId option_id = ui_id("mutator_option", option_key(mutator, i));
        const auto rect = form_scroll_row(ctx, scroll, x + indent, y, row_h, option_id, option_focus_kind(option));
        if (rect) {
            if (build_option(ctx, *rect, mutator, i, values[i], fonts, prev_text_focus, result)) {
                values[i].player_set = true;
                result.changed = true;
            }
        }
    }
    return result;
}

const char* mutator_description(std::string_view name)
{
    for (const auto& entry : mutator_descriptions) {
        if (name == entry.name) {
            return entry.text;
        }
    }
    return "No description available.";
}
