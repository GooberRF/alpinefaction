#pragma once

#include <chrono>
#include <string>
#include <vector>
#include "column_menu.h"
#include "../rf/multi.h"

class MenuPopup;

// The Join Game browser as a panel of a column menu: the Multi menu's in GS_MULTI_SERVER_LIST, and the in-game
// multiplayer menu's over a live game. Its dialogs go on the host's popup. The tab, filters, sort and selection last
// the session.
class ServerBrowserPanel final : public ColumnPanel
{
public:
    explicit ServerBrowserPanel(MenuPopup& popup) : popup_{popup} {}

    [[nodiscard]] bool is_open() const override
    {
        return open_;
    }
    void open() override;
    void close() override;
    bool back() override;
    void build(UiContext& ctx, const MenuPanel& panel, const MenuFonts& fonts) override;
    void commit() override;
    bool key(UiContext& ctx, int key) override;
    void hints(const UiContext& ctx, std::vector<MenuHint>& hints) const override;

    // Once a frame while the host runs, ahead of its input pass.
    void frame();

private:
    using Clock = std::chrono::steady_clock;
    // Longer than the shared delay: sweeping the cursor down the list would otherwise flash a card on every row.
    static constexpr std::chrono::milliseconds card_delay{600};

    enum class Action
    {
        join,
        get_servers,
        refresh_all,
        add_server,
        favorite,
    };

    void refresh_view();
    [[nodiscard]] int selected_entry() const;
    [[nodiscard]] int selected_row() const;
    // The server the details card shows, -1 for none.
    [[nodiscard]] int card_entry() const;
    void move_selection(int to_row);
    void run(Action action);
    void request_join(int entry);
    void confirm_join(const rf::NetAddr& addr, const std::string& name, const std::string& password);
    void toggle_favorite(int entry);
    void notice(const char* title, std::string text);

    MenuPopup& popup_;
    bool open_ = false;
    // Indices into server_browser_entries(), filtered and sorted; entries are only appended, so they stay valid.
    std::vector<int> view_;
    // The user changed the filter, the sort or the list, which shows at once even under the cursor.
    bool rebuild_view_ = false;
    int top_ = 0;
    int visible_ = 1;
    bool dragging_ = false;
    bool scroll_to_selection_ = false;
    // The keyboard moved the selection last, so the card follows it.
    bool keyboard_selection_ = false;
    int last_click_entry_ = -1;
    Clock::time_point last_click_{};
    MenuHoverRest<int> hover_{-1, card_delay};
    // card_entry() at the last frame(), -1 for none: a card coming up asks for its players.
    int card_shown_ = -1;
    // Taken by the hit-test pass: the cursor is on the panel's controls rather than the list.
    bool pointer_on_controls_ = false;
    // Taken by the hit-test pass, run after it.
    std::vector<Action> actions_;
    int favorite_toggle_ = -1;
};
