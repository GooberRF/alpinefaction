#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "menu_shell.h"
#include "ui_kit.h"
#include "../rf/gr/gr.h"

// The rows of the Options menu, for any screen whose rows should read like them: a label on the left over a ridge,
// its control right-aligned in a column skin::control_w wide. Everything takes the hit-test / draw passes of ui_kit,
// inside a scroll region or not.

// Left inset of a row's label and right inset of its control.
inline constexpr float form_row_inset = 32.0f;
inline constexpr float form_sub_header_h = 40.0f;
// Choices with at most this many values cycle in place; longer ones get a dropdown list.
inline constexpr int form_cycler_max_options = 4;

struct FormFonts
{
    int body = -1;
    int body_bold = -1;
    int caption = -1;
};
// A menu screen's fonts as its rows take them.
FormFonts form_fonts(const MenuFonts& fonts);

// A chip beside the label.
struct FormBadge
{
    const char* text = "";
    rf::Color color{};
    const char* icon = nullptr;
    bool lamp = false;
};

// A sub-option's place under the row it belongs to, drawn as a branch off that row.
enum class FormTree
{
    None,
    Branch, // another sub-option follows
    Last,
};

struct FormRow
{
    UiRect rect;
    // The control's: its keyboard focus lights the row.
    UiId id = 0;
    const char* label = "";
    // A second line under the label, which needs form_crumb_line_h more than a one-line row's height.
    const char* crumb = nullptr;
    bool enabled = true;
    // The amber lamp and a lit label, and while enabled the reset icon `reset_id`, if any.
    bool modified = false;
    UiId reset_id = 0;
    std::span<const FormBadge> badges;
    // Hovering or focusing the row shows it in ctx.description(), and a click pins it; -1 for none.
    int desc_id = -1;
    // A sub-option's label is indented, behind a branch down from the row above.
    FormTree tree = FormTree::None;
};

// Where a row's control goes and how it shows.
struct FormLine
{
    UiRect rect;
    int ctl_right = 0;
    bool enabled = true;
    // Hovered or focused, which brightens the value.
    bool hot = false;
    // The reset icon was pressed (hit-test pass).
    bool reset = false;
};

// The row less its control, which the caller builds next on the line it returns.
FormLine form_row(UiContext& ctx, const FormRow& row, const FormFonts& fonts);
// The height a crumb in `font` (the rows' caption font) adds under the label.
int form_crumb_line_h(const UiContext& ctx, int font);
// The keyboard-focused row's selection, else the ridge under the row. `d` is draw space.
void form_draw_row_backing(const UiContext& ctx, const UiRect& d, bool focused);
rf::Color form_label_color(bool enabled, bool hot, bool focused);
// A group's heading, with its lamp.
void form_draw_sub_header(const UiContext& ctx, const UiRect& r, const char* text, int font);
// A control `w_ref` by `h_ref` reference pixels right-aligned on the line, centered on it; `h_ref` 0 takes the row's
// height.
UiRect form_control_rect(const UiContext& ctx, const FormLine& line, float w_ref, float h_ref);

// Controls. Results come back in the hit-test pass only. A number a widget edits in place must outlive the frame: a
// typed value is committed from a later pass.

// The step taken: -1, 0 or +1.
int form_cycler(UiContext& ctx, const FormLine& line, UiId id, const char* value, int font);
// On / Off. True when it flipped `on`.
bool form_toggle(UiContext& ctx, const FormLine& line, UiId id, bool& on, int font);
// One of `items` by `index`: cycled in place when there are few, else a dropdown. True when `index` changed.
bool form_choice(UiContext& ctx, const FormLine& line, UiId id, const std::vector<std::string>& items, int& index,
                 int font);
bool form_dropdown(UiContext& ctx, const FormLine& line, UiId id, const std::vector<std::string>& items, int& index,
                   int font);
// A closed dropdown's look for a choice made elsewhere. True when pressed.
bool form_select_box(UiContext& ctx, const FormLine& line, UiId id, const char* text, int font);
bool form_slider(UiContext& ctx, const FormLine& line, UiId id, float& value, float min_value, float max_value,
                 float step, float nudge, const UiNumberFormat& fmt, int font, int readout_w);
bool form_stepper(UiContext& ctx, const FormLine& line, UiId id, float& value, float min_value, float max_value,
                  float step, const UiNumberFormat& fmt, int font, bool integral);

struct FormTextSpec
{
    std::size_t max_len = 63;
    UiCharFilter filter = ui_char_filter_filename;
    const char* hint = "";
    // Set while the text cannot be committed.
    const char* warn = nullptr;
};
// A box editing `text` in place, which the caller keeps across frames and refreshes from its value while no box is
// being edited. `on_commit` runs when the box lets go on Enter, a click away or the row leaving the pass; Esc skips
// it, so the refresh puts the value back. `prev_text_focus` is the text focus the caller noted after its previous
// hit-test pass.
void form_text(UiContext& ctx, const FormLine& line, UiId id, std::string& text, const FormTextSpec& spec, int font,
               UiId prev_text_focus, const std::function<void()>& on_commit);
// A number in a box: `shown` while the box is idle, `seed` when an edit starts, and `on_commit` with the typed text
// when the box lets go, as form_text. True on the pass the box takes the focus.
bool form_number_text(UiContext& ctx, const FormLine& line, UiId id, const std::string& shown, const std::string& seed,
                      const FormTextSpec& spec, int font, UiId prev_text_focus,
                      const std::function<void(const std::string&)>& on_commit);
// A read-only value where the control goes.
void form_value_text(const UiContext& ctx, const FormLine& line, const char* text, int font);

// The strip under the rows that describes the hovered one.
void form_draw_description(const UiContext& ctx, const UiRect& strip, const std::string& text, int font);

// Where a panel's parts go: the body, the description strip under it and the footer line. A panel without a footer
// gives its space to the strip and the body.
struct FormPanelAreas
{
    UiRect body;
    UiRect desc;
    UiRect footer;
};
FormPanelAreas form_panel_areas(const UiContext& ctx, const MenuPanel& panel, bool with_desc, bool with_footer = true);
// Rows start left of the content edge, so a focused row's LED lines up with the heading's.
UiRect form_rows_view(const UiContext& ctx, const UiRect& area);

// Rows in a scrolling view. A row out of view is not built; the caller registers its control as focusable instead,
// so the keyboard can still step onto it, and notes the focused row's rect, which scrolls into view once the keyboard
// moved onto it.
struct FormScroll
{
    UiScrollRegion region;
    // The rows' width, clear of the scrollbar.
    int inner_w = 0;
    std::optional<UiRect> focus_rect;
};
FormScroll form_scroll_begin(UiContext& ctx, const UiRect& view, int content_h, float& scroll, int wheel_step, UiId id);
bool form_scroll_culled(const FormScroll& scroll, const UiRect& row);
// The next row, `h` tall at `y` across the rows' width from `x`, which `y` then moves past. Empty when it is out of
// view, where the control `id` is registered as `kind` instead; 0 for a row with nothing to focus.
std::optional<UiRect> form_scroll_row(UiContext& ctx, FormScroll& scroll, int x, int& y, int h, UiId id,
                                      UiFocusKind kind);
// `scrolled_to` is the caller's record of the focus it last scrolled to.
void form_scroll_end(UiContext& ctx, FormScroll& scroll, UiId& scrolled_to);
