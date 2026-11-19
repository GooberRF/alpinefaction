#include <algorithm>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "column_menu.h"
#include "../rf/gameseq.h"
#include "../rf/sound/sound.h"
#include "../rf/ui.h"
#include "../sound/sound.h"

namespace
{

// The crumbs of the columns that handed off to the screen running now, outermost first: a hand-off's frame runs
// inside its host's host_frame.
std::vector<std::string> g_host_crumbs;

// Where a click on the path strip leads back to: the menu state at index `state` of the state stack, and within it the
// screen `host` hand-offs deep. Going to the player, it ends early at the first screen opening Multi > Player.
struct MenuJump
{
    int state = -1;
    rf::GameState state_type = rf::GS_NONE;
    std::size_t host = 0;
    bool player = false;
};
std::optional<MenuJump> g_jump;

void start_jump(const MenuCrumbTarget& target, bool player)
{
    if (target.state >= 0 && target.state <= rf::gameseq_stack_top_index
        && target.state < static_cast<int>(std::size(rf::gameseq_state_stack))) {
        g_jump = MenuJump{target.state, rf::gameseq_state_stack[target.state], target.host, player};
    }
}

// The jump under way, dropped once the state it leads to has gone, as after a state change it did not make.
const MenuJump* active_jump()
{
    if (g_jump && (g_jump->state > rf::gameseq_stack_top_index
                   || rf::gameseq_state_stack[g_jump->state] != g_jump->state_type)) {
        g_jump.reset();
    }
    return g_jump ? &*g_jump : nullptr;
}

// The screen running now is above where `jump` leads.
bool above_jump(const MenuJump& jump)
{
    const int own = menu_own_state();
    return own > jump.state || (own == jump.state && g_host_crumbs.size() > jump.host);
}

} // namespace

void column_menu_jump_to_step(std::size_t step)
{
    start_jump(menu_crumb_target(step), false);
}

void column_menu_jump_to_player()
{
    start_jump(menu_crumb_target(0), true);
}

void column_menu_cancel_jump()
{
    g_jump.reset();
}

bool column_menu_player_jump()
{
    const MenuJump* jump = active_jump();
    return jump && jump->player;
}

void ColumnMenu::enter(std::string title, std::vector<ColumnButton> buttons, ColumnButton back, int esc_action,
                       bool slide_in_sound)
{
    title_ = std::move(title);
    crumb_ = title_;
    esc_hint_ = "Back";
    nav_hints_ = true;
    set_buttons(std::move(buttons));
    back_ = std::move(back);
    esc_action_ = esc_action;
    player_action_.reset();
    strip_clicks_ = true;
    slide_in_sound_ = slide_in_sound;
    panels_.clear();
    closing_ = false;
    hand_off_ = false;
    hand_off_to_ = {};
    slide_ = 0.0f;
    shown_ = nullptr;
    panel_slide_ = 0.0f;
    pending_.reset();
    popup_.close();
    menu_ui_reset(ui_);
}

void ColumnMenu::add_panel(ColumnPanel* panel, int action)
{
    panels_.push_back(Panel{panel, action});
}

void ColumnMenu::set_buttons(std::vector<ColumnButton> buttons)
{
    // BACK takes the slot under them.
    assert(static_cast<int>(buttons.size()) + 1
           <= (button_size_ == MenuButtonSize::Large ? menu_large_slots : menu_panel_slots));
    buttons_ = std::move(buttons);
    items_.clear();
    for (const ColumnButton& button : buttons_) {
        items_.push_back(MenuColumnItem{button.label, false, button.opens_panel, button.enabled});
    }
}

ColumnPanel* ColumnMenu::open_panel_now() const
{
    for (const Panel& p : panels_) {
        if (p.panel->is_open()) {
            return p.panel;
        }
    }
    return nullptr;
}

ColumnPanel* ColumnMenu::modal_panel() const
{
    ColumnPanel* open = open_panel_now();
    return open && open == shown_ && open->modal_open() ? open : nullptr;
}

int ColumnMenu::panel_slot(const ColumnPanel* panel) const
{
    const auto p = std::ranges::find(panels_, panel, &Panel::panel);
    if (!panel || p == panels_.end()) {
        return -1;
    }
    const auto button = std::ranges::find(buttons_, p->action, &ColumnButton::action);
    return button == buttons_.end() ? -1 : static_cast<int>(button - buttons_.begin());
}

void ColumnMenu::open_panel(ColumnPanel* panel)
{
    for (const Panel& p : panels_) {
        if (p.panel != panel && p.panel->is_open()) {
            p.panel->close();
        }
    }
    if (!panel->is_open()) {
        panel->open();
    }
}

void ColumnMenu::toggle_panel(ColumnPanel* panel)
{
    if (panel->is_open()) {
        panel->close();
    }
    else {
        open_panel(panel);
    }
}

void ColumnMenu::seat_panel(ColumnPanel* panel)
{
    open_panel(panel);
    shown_ = panel;
    panel_slide_ = 1.0f;
}

ColumnPanel* ColumnMenu::shown_panel() const
{
    return panel_slide_ > 0.0f ? shown_ : nullptr;
}

bool ColumnMenu::host_frame(bool paused)
{
    const auto run_hand_off = [&] {
        g_host_crumbs.push_back(crumb_);
        hand_off_to_.do_frame(paused);
        g_host_crumbs.pop_back();
    };
    if (hand_off_to_.is_active && hand_off_to_.is_active()) {
        run_hand_off();
        return true;
    }
    step();
    if (!hand_off_ || slide_ > 0.0f) {
        return false;
    }
    hand_off_ = false;
    hand_off_to_.open();
    run_hand_off();
    return true;
}

void ColumnMenu::step()
{
    const float step = menu_slide_step();
    // A jump passes the screens beneath unseen. Going to the player it holds even the one opening Multi > Player,
    // which may hand off or close at once.
    const MenuJump* jump = active_jump();
    const bool held = jump && (jump->player || above_jump(*jump));
    slide_ = closing_ || hand_off_ || held ? std::max(0.0f, slide_ - step) : std::min(1.0f, slide_ + step);
    if (slide_ > 0.0f && std::exchange(slide_in_sound_, false)) {
        rf::snd_play(stock_sound_id::menu_slide_in, 0, 0.0f, 1.0f);
    }
    ColumnPanel* want = open_panel_now();
    if (shown_ != want && panel_slide_ <= 0.0f) {
        shown_ = want;
        // Description ids are each panel's own.
        ui_.description() = UiDescriptionState{};
    }
    const bool in = shown_ && shown_ == want;
    panel_slide_ = in ? std::min(1.0f, panel_slide_ + step) : std::max(0.0f, panel_slide_ - step);
}

void ColumnMenu::build(UiContext& ctx)
{
    const MenuFrame frame = menu_frame(ctx, slide_);
    const MenuLayout& lay = frame.lay;
    if (ctx.draw && !title_.empty()) {
        menu_draw_title_bar(ctx, lay.panel_x + frame.enter_dx, lay.panel_r + frame.enter_dx, frame.canvas_y,
                            title_.c_str());
    }

    const int lit_slot = panel_slot(lit_panel());
    for (std::size_t i = 0; i < items_.size(); ++i) {
        items_[i].selected = static_cast<int>(i) == lit_slot;
    }
    const int clicked = menu_build_column(ctx, lay, lay.column_x + frame.col_dx, frame.top, items_,
                                          back_.label.c_str(), frame.fonts.column, "column", button_size_);
    if (clicked >= 0 && !ctx.draw) {
        pending_ = clicked < static_cast<int>(buttons_.size()) ? buttons_[clicked].action : back_.action;
    }

    if (shown_ && panel_slide_ > 0.0f && (ctx.draw || shown_->is_open())) {
        const int panel_dx =
            frame.enter_dx + static_cast<int>((1.0f - panel_slide_) * static_cast<float>(frame.slide_w));
        const MenuPanel panel = menu_panel(ctx, lay, panel_dx, frame.top, frame.canvas_y);
        if (ctx.draw) {
            menu_draw_panel(ctx, panel);
            if (const int slot = panel_slot(shown_); slot >= 0) {
                menu_draw_connector(ctx, panel, frame.top + menu_slot_mid_y(ctx, button_size_, slot));
            }
        }
        shown_->build(ctx, panel, frame.fonts);
    }

    if (ctx.draw) {
        const std::vector<std::string> crumbs = menu_crumbs(g_host_crumbs);
        menu_draw_frame(ctx, path(crumbs), hints(), strip_links(crumbs.size()));
    }
}

ColumnPanel* ColumnMenu::lit_panel() const
{
    // A panel on its way out keeps its button lit until the next one comes in.
    ColumnPanel* lit = open_panel_now();
    if (lit && shown_ && shown_ != lit && panel_slide_ > 0.0f) {
        lit = shown_;
    }
    return lit;
}

std::vector<std::string> ColumnMenu::path(std::vector<std::string> crumbs) const
{
    // The path ends with the screen and the tab of its lit button.
    std::vector<std::string> path = std::move(crumbs);
    path.push_back(crumb_);
    if (const int lit_slot = panel_slot(lit_panel()); lit_slot >= 0) {
        path.push_back(buttons_[lit_slot].label);
    }
    return path;
}

MenuStripLinks ColumnMenu::strip_links(std::size_t crumbs) const
{
    // A dialog takes the clicks, and a slide out takes none, as input().
    if (!strip_clicks_ || !seated() || closing_ || hand_off_ || popup_.active() || modal_panel()
        || rf::ui::popup_is_active()) {
        return {};
    }
    // The menus above the screen, not the screen and its tab. A jump leaves by Esc's action, so a screen whose BACK is
    // not that, as the main menu's Quit, starts none.
    const bool closes = closes_for_jump();
    return MenuStripLinks{closes ? crumbs : 0, menu_player_reachable() && (closes || player_action_)};
}

std::vector<MenuHint> ColumnMenu::hints() const
{
    std::vector<MenuHint> hints;
    // A dialog dims the hint bar, and takes the keys itself. A slide out takes none, as input().
    if (popup_.active() || modal_panel() || rf::ui::popup_is_active() || closing_ || hand_off_) {
        return hints;
    }
    menu_add_nav_hints(ui_, hints, esc_hint_, nav_hints_);
    if (ColumnPanel* open = open_panel_now()) {
        open->hints(ui_, hints);
    }
    return hints;
}

void ColumnMenu::key(int key)
{
    if (key == rf::KEY_NONE) {
        return;
    }
    if (popup_.active()) {
        popup_.key(ui_, key);
        return;
    }
    // A focused text box owns everything; the context replays it at the start of the next hit-test pass.
    if (ui_.focused_text_id() != 0) {
        ui_.push_key(static_cast<rf::Key>(key));
        return;
    }
    const int code = key & rf::KEY_MASK;
    if (ColumnPanel* modal = modal_panel()) {
        // Esc closes an open dropdown, not the dialog it is in.
        if (code == rf::KEY_ESC && ui_.popup_id() != 0) {
            ui_.close_popup();
            ui_play_click();
        }
        else if (ui_.popup_id() != 0 || !modal->modal_key(ui_, key)) {
            ui_.handle_nav_key(static_cast<rf::Key>(key));
        }
        return;
    }
    ColumnPanel* open = open_panel_now();
    if (code == rf::KEY_ESC) {
        if (ui_.popup_id() != 0) {
            ui_.close_popup();
            ui_play_click();
            return;
        }
        if (open && open->back()) {
            ui_play_click();
            return;
        }
        pending_ = esc_action_;
        return;
    }
    // An open dropdown owns the keys, so no hotkey acts on the screen under it.
    if (ui_.popup_id() != 0) {
        ui_.handle_nav_key(static_cast<rf::Key>(key));
        return;
    }
    if (open && open->key(ui_, key)) {
        return;
    }
    // Hotkeys need no modifier held, as stock's do.
    if (key == code) {
        if (back_.hotkey != rf::KEY_NONE && code == back_.hotkey) {
            pending_ = back_.action;
            return;
        }
        for (const ColumnButton& button : buttons_) {
            if (button.enabled && button.hotkey != rf::KEY_NONE && code == button.hotkey) {
                pending_ = button.action;
                return;
            }
        }
    }
    ui_.handle_nav_key(static_cast<rf::Key>(key));
}

std::optional<int> ColumnMenu::input(void (*key_callback)(int))
{
    jump_action_ = false;
    if (closing_ || hand_off_) {
        return std::nullopt;
    }
    if (const MenuJump* jump = active_jump()) {
        const bool player = jump->player && player_action_;
        // A screen its BACK would not close, as the main menu's Quit, ends the jump where it is.
        if (above_jump(*jump) && !player && closes_for_jump()) {
            jump_action_ = true;
            return esc_action_;
        }
        g_jump.reset();
        if (player) {
            jump_action_ = true;
            return player_action_;
        }
    }
    // As drawn last frame, before the pass's clicks change it.
    const std::vector<std::string> crumbs = menu_crumbs(g_host_crumbs);
    const MenuStripLinks links = strip_links(crumbs.size());
    const std::vector<std::string> shown_path = path(crumbs);
    menu_run_hit_test(ui_, key_callback, [this](UiContext& ctx) {
        if (popup_.active()) {
            popup_.build(ctx);
            return;
        }
        ctx.description().begin_pass();
        if (ColumnPanel* modal = modal_panel()) {
            modal->build_modal(ctx);
        }
        else {
            build(ctx);
        }
        ctx.description().end_pass();
    });
    popup_.commit();
    if (shown_ && !popup_.active()) {
        shown_->commit();
    }
    if (!pending_) {
        const int link = menu_strip_click(ui_, shown_path, links);
        if (link == menu_strip_player && player_action_) {
            pending_ = player_action_;
        }
        else if (link != -1) {
            if (link == menu_strip_player) {
                column_menu_jump_to_player();
            }
            else {
                column_menu_jump_to_step(static_cast<std::size_t>(link));
            }
            pending_ = esc_action_;
        }
    }
    return std::exchange(pending_, std::nullopt);
}

void ColumnMenu::draw(bool backdrop)
{
    menu_run_draw(
        ui_,
        [this](UiContext& ctx) {
            ColumnPanel* const modal = modal_panel();
            // The screen under a dialog takes no input, so nothing on it lights up under the cursor.
            const int mouse_x = ctx.mx;
            const int mouse_y = ctx.my;
            if (popup_.active() || modal || rf::ui::popup_is_active()) {
                ctx.set_mouse_pos(-1, -1);
            }
            build(ctx);
            ctx.set_mouse_pos(mouse_x, mouse_y);
            if (modal) {
                modal->build_modal(ctx);
            }
            popup_.build(ctx);
        },
        backdrop);
}
