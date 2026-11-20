#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "ui_kit.h"
#include "../rf/input.h"

// A modal message or text prompt in the dialog look of the settings menu, over a menu screen or over the game. While
// it is active the host routes its keys to key(), builds it alone in the hit-test pass and last in the draw pass, and
// calls commit() after the hit-test pass.

struct MenuPopupButton
{
    std::string label;
    rf::Key hotkey = rf::KEY_NONE;
};

enum class MenuPopupStyle
{
    // The cancel button Secondary, the ones it confirms Danger.
    Confirm,
    // Every button Secondary, for plain choices.
    Neutral,
};

class MenuPopup
{
public:
    // The button pressed, and for a prompt the text typed.
    using Done = std::function<void(int button, const std::string& text)>;

    // `cancel` is the button Esc presses, -1 for none. `default_button` is the one the keyboard focus starts on, lit,
    // and Enter presses while no other has the focus.
    void open(std::string title, std::string text, std::vector<MenuPopupButton> buttons, int cancel,
              int default_button, Done done, MenuPopupStyle style = MenuPopupStyle::Confirm);
    // `prompt` over a text box taking the typing, then CANCEL (0) and `ok_label` (1), which Enter presses.
    void open_input(std::string title, std::string prompt, std::string initial, size_t max_len, UiCharFilter filter,
                    std::string ok_label, Done done);
    void close();
    [[nodiscard]] bool active() const
    {
        return open_;
    }

    // Consumes every key while active.
    bool key(UiContext& ctx, int key);
    void build(UiContext& ctx);
    // Runs `done` for the button the hit-test pass pressed, once the popup has closed.
    void commit();

private:
    [[nodiscard]] int focused_button(const UiContext& ctx) const;
    // The focused button, else the default.
    [[nodiscard]] int current_button(const UiContext& ctx) const;
    void seat_focus(UiContext& ctx);

    bool open_ = false;
    std::string title_;
    std::string text_;
    std::vector<MenuPopupButton> buttons_;
    int cancel_ = -1;
    int default_ = -1;
    MenuPopupStyle style_ = MenuPopupStyle::Confirm;
    Done done_;
    bool input_ = false;
    bool focus_input_ = false;
    size_t input_max_ = 0;
    UiCharFilter input_filter_ = nullptr;
    std::string input_text_;
    std::optional<int> pressed_;
    // The screen's focus, taken as the popup seats its own and given back as it closes.
    bool seat_focus_ = false;
    UiId screen_focus_ = 0;
    bool screen_focus_visible_ = false;
    bool screen_focus_saved_ = false;
};

// Stock's popups drawn and clicked in the MenuPopup look, unless legacy menus are on. Their text, buttons, keys and
// callbacks stay stock's.
void menu_popup_apply_patch();
