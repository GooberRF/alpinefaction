#pragma once

#include <optional>
#include <string>
#include <vector>
#include "menu_popup.h"
#include "menu_shell.h"
#include "ui_kit.h"
#include "../rf/input.h"

// A menu screen in the new style: the button column sliding in from under the left bar, the title bar, the path strip,
// the hint bar's keys, letter hotkeys, panels beside the column that slide in and out, a popup over it all, and the
// slide out before the state closes.

struct ColumnButton
{
    int action = 0;
    std::string label;
    rf::Key hotkey = rf::KEY_NONE;
    // The arrow of a button that opens a panel.
    bool opens_panel = false;
    // Disabled, it is dimmed and takes no click or hotkey.
    bool enabled = true;
};

// The contents of a panel beside the column, opened by one of its buttons.
class ColumnPanel
{
public:
    virtual ~ColumnPanel() = default;
    [[nodiscard]] virtual bool is_open() const = 0;
    virtual void open() = 0;
    // Another of the column's panels opens.
    virtual void close() = 0;
    // Esc while open: up a level or closed, true; false leaves Esc to the column.
    virtual bool back() = 0;
    // Either pass, inside the frame the column drew. A panel on its way out is only painted.
    virtual void build(UiContext& ctx, const MenuPanel& panel, const MenuFonts& fonts) = 0;
    // Runs what the hit-test pass took, before the column's action.
    virtual void commit() {}
    // A key while open, ahead of the column's letters; false leaves it to the column.
    virtual bool key(UiContext&, int)
    {
        return false;
    }
    // A dialog of the panel's own. While it is up the screen is only painted, and the dialog takes the keys, Esc
    // included, and the clicks.
    [[nodiscard]] virtual bool modal_open() const
    {
        return false;
    }
    // Either pass, over the frame.
    virtual void build_modal(UiContext&) {}
    // False leaves the key to keyboard navigation.
    virtual bool modal_key(UiContext&, int)
    {
        return false;
    }
    // The hint bar's keys while open, after the column's: the panel's own added, and those it takes over relabeled
    // or dropped.
    virtual void hints(const UiContext&, std::vector<MenuHint>&) const {}
};

// A click on the path strip goes back up the menus: each screen above where it leads closes by its BACK, as Esc and
// BACK close it, the screens beneath being held off canvas on the way. These start that for a screen not built on
// ColumnMenu, which closes itself: back to step `step` of a path menu_crumbs({}) begins, or for the player up to the
// first screen that opens Multi > Player.
void column_menu_jump_to_step(std::size_t step);
void column_menu_jump_to_player();
// For menus closed without their BACK, as a level change closes them.
void column_menu_cancel_jump();
// A click on the path strip's player is on its way up the menus.
bool column_menu_player_jump();

// A screen that opens in a column's place, run within the column's state.
struct ColumnHandOff
{
    void (*open)() = nullptr;
    // From the open until its exit animation is done.
    bool (*is_active)() = nullptr;
    // `paused`: a stock popup owns input this frame; it renders after us and on top.
    void (*do_frame)(bool paused) = nullptr;
};

class ColumnMenu
{
public:
    // Large buttons hold menu_large_slots, BACK included; small ones menu_panel_slots.
    explicit ColumnMenu(MenuButtonSize button_size = MenuButtonSize::Large) : button_size_{button_size} {}

    // A fresh context, and the slide in from off canvas. An empty `title` draws no title bar. `back` takes the slot
    // under the buttons; Esc with no panel to go back takes `esc_action`.
    void enter(std::string title, std::vector<ColumnButton> buttons, ColumnButton back, int esc_action,
               bool slide_in_sound = true);
    // After enter, which forgets them: the panel the button with `action` opens.
    void add_panel(ColumnPanel* panel, int action);
    // After enter, which sets it to the title: the screen's step in the path, its own and the screens it hands off to.
    void set_crumb(std::string crumb)
    {
        crumb_ = std::move(crumb);
    }
    // After enter, which sets it to Back with the arrows and Enter: Esc's hint, empty for none, and without `nav` where
    // only Esc and the typing reach the screen.
    void set_esc_hint(std::string label, bool nav = true)
    {
        esc_hint_ = std::move(label);
        nav_hints_ = nav;
    }
    // After enter, for a screen that opens Multi > Player: the action a click on the path strip's player takes.
    // Elsewhere the click goes back up the menus to such a screen.
    void set_player_action(int action)
    {
        player_action_ = action;
    }
    // After enter, which turns them on: off where the frame shows over the game.
    void set_strip_clicks(bool on)
    {
        strip_clicks_ = on;
    }
    // The buttons over BACK replaced in place, the slides and panels kept.
    void set_buttons(std::vector<ColumnButton> buttons);
    // Closes the column's other panels. The one on screen slides out before the next slides in.
    void open_panel(ColumnPanel* panel);
    void toggle_panel(ColumnPanel* panel);
    // Opens `panel` already seated, to come in with the screen.
    void seat_panel(ColumnPanel* panel);
    // Open, or on its way out.
    [[nodiscard]] ColumnPanel* shown_panel() const;

    // The caller changes the state.
    void request_close()
    {
        closing_ = true;
    }
    [[nodiscard]] bool closing() const
    {
        return closing_;
    }
    // Slid in all the way.
    [[nodiscard]] bool seated() const
    {
        return slide_ >= 1.0f;
    }
    // Closing, and slid out.
    [[nodiscard]] bool is_closed() const
    {
        return closing_ && slide_ <= 0.0f;
    }
    // Slides out for `screen`, which opens in its place, and back in once that screen is gone.
    void hand_off(const ColumnHandOff& screen)
    {
        hand_off_ = true;
        hand_off_to_ = screen;
    }
    // Once a frame, before input(): steps the slides, opens the screen handed off to once the column has slid out,
    // and gives that screen the frame while it is active. True when the screen had the frame. `paused` as for
    // ColumnHandOff::do_frame.
    bool host_frame(bool paused);

    // The slides, once a frame.
    void step();
    // A frame's hit-test pass. `key_callback` forwards to key(); null for a host that calls key() itself. Returns
    // the action taken; none while closing or handing off.
    std::optional<int> input(void (*key_callback)(int));
    void key(int key);
    // A frame's draw pass, over the stock menu background unless `backdrop` is false.
    void draw(bool backdrop = true);
    // The action input() last returned takes a jump past the screen, or ends it: the click that started the jump had
    // its sound, so the handler plays none.
    [[nodiscard]] bool jump_action() const
    {
        return jump_action_;
    }

    MenuPopup& popup()
    {
        return popup_;
    }
    UiContext& ui()
    {
        return ui_;
    }

private:
    struct Panel
    {
        ColumnPanel* panel = nullptr;
        int action = 0;
    };

    void build(UiContext& ctx);
    [[nodiscard]] std::vector<MenuHint> hints() const;
    // The panel whose button is lit, and the path ending with the screen and that button after `crumbs`, menu_crumbs
    // of the screen's hosts.
    [[nodiscard]] ColumnPanel* lit_panel() const;
    [[nodiscard]] std::vector<std::string> path(std::vector<std::string> crumbs) const;
    [[nodiscard]] MenuStripLinks strip_links(std::size_t crumbs) const;
    // Esc's action, which a jump closes the screen with, is BACK's.
    [[nodiscard]] bool closes_for_jump() const
    {
        return esc_action_ == back_.action;
    }
    [[nodiscard]] ColumnPanel* open_panel_now() const;
    [[nodiscard]] ColumnPanel* modal_panel() const;
    [[nodiscard]] int panel_slot(const ColumnPanel* panel) const;

    MenuButtonSize button_size_;
    UiContext ui_;
    MenuPopup popup_;
    std::string title_;
    std::string crumb_;
    std::string esc_hint_;
    bool nav_hints_ = true;
    std::vector<ColumnButton> buttons_;
    std::vector<MenuColumnItem> items_;
    ColumnButton back_;
    int esc_action_ = 0;
    std::optional<int> player_action_;
    bool strip_clicks_ = true;
    // Played once the column first moves in.
    bool slide_in_sound_ = false;
    std::vector<Panel> panels_;
    bool closing_ = false;
    bool hand_off_ = false;
    ColumnHandOff hand_off_to_;
    // 0 = off canvas, 1 = seated.
    float slide_ = 0.0f;
    // The panel on screen and its slide, which follows whether it is open.
    ColumnPanel* shown_ = nullptr;
    float panel_slide_ = 0.0f;
    // Taken at the end of the input pass, so a handler never runs with the pass half built.
    std::optional<int> pending_;
    bool jump_action_ = false;
};
