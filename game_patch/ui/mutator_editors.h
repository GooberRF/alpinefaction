#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "form_view.h"
#include "ui_kit.h"
#include "../multi/server_internal.h"

// A mutator as Options-style rows: its on/off row, and under it while on the rows of its options. The caller owns
// the values and describes the mutator from whichever schema it has, the vote options the server sent or the
// mutator registry.

// One option's value, whatever its type.
struct MutatorEditorValue
{
    bool bool_value = false;
    uint8_t choice_index = 0;
    int32_t int_value = 0;
    float float_value = 0.0f;
    // The player set it, so a default derived later leaves it alone.
    bool player_set = false;
};

struct MutatorEditorOption
{
    const char* label = "";
    MutatorOptionType type = MutatorOptionType::Bool;
    // Choice only.
    const std::vector<std::string>* choices = nullptr;
    // An own value over an inherited one: the lamp, and the reset icon.
    bool modified = false;
};

struct MutatorEditor
{
    // Unique among the screen's mutators and stable across passes; the rows' ids derive from it.
    int key = 0;
    const char* label = "";
    // What hovering any of its rows shows in ctx.description(); -1 for none.
    int desc_id = -1;
    // Off grays the on/off row and hides the options, for a game type the mutator does nothing in.
    bool enabled = true;
    // The on/off row's lamp and reset icon.
    bool modified = false;
    std::span<const MutatorEditorOption> options;
};

struct MutatorEditorResult
{
    // Switched, or an option edited (hit-test pass).
    bool changed = false;
    // The on/off row's reset icon was pressed.
    bool reset = false;
    // The option whose reset icon was pressed, -1 for none.
    int reset_option = -1;
};

// What the mutator `name`, its registry name, does.
const char* mutator_description(std::string_view name);

// The height mutator_editor_rows takes.
int mutator_editor_height(const UiContext& ctx, const MutatorEditor& mutator, bool on);
// The rows at `y` from `x`, which `y` moves past. `values` lines up with `mutator.options`, and must not move while a
// box edits one: a typed number lands on a later pass.
MutatorEditorResult mutator_editor_rows(UiContext& ctx, FormScroll& scroll, int x, int& y, const MutatorEditor& mutator,
                                        bool& on, std::span<MutatorEditorValue> values, const FormFonts& fonts,
                                        UiId prev_text_focus);
