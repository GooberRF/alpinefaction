#pragma once

#include <optional>
#include <chrono>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "column_menu.h"
#include "form_view.h"
#include "menu_shell.h"
#include "settings_skin.h"
#include "../multi/listen_server_setup.h"
#include "../multi/mutators.h"
#include "../rf/multi.h"

// What Host Game's panels share. The setup is the screen's state: every row reads it each pass and writes to it
// as it changes.

struct CreateGameMap
{
    std::string filename; // with ".rfl"
    std::string name;
};

struct CreateGameState
{
    ListenServerSetup setup;
    // The game type a map added next runs; the map list's filter follows it when it changes.
    rf::NetGameType game_type = rf::NG_TYPE_DM;
    std::vector<CreateGameMap> maps;
    // The Join Bots presets a bot added next starts with, as indices into their choices.
    int bot_skill = -1;
    int bot_personality = -1;
    // The text focus after the previous hit-test pass.
    UiId prev_text_focus = 0;
};

CreateGameState& create_game();
inline ListenServerSetup& create_game_setup()
{
    return create_game().setup;
}
void create_game_notice(std::string title, std::string text);
bool create_game_popup_active();

std::string create_game_map_name(std::string_view filename);
// The rotation's game types, or the Game Type while it is empty.
std::vector<rf::NetGameType> create_game_types_in_play();
bool create_game_any_type_in(uint32_t game_type_mask);
bool create_game_bots_in_play();
bool create_game_teams_in_play();
// Every slot but the host's.
int create_game_bots_limit();
// Join Bots' presets, which a bot profile names.
const std::vector<MutatorOptionChoice>& create_game_bot_skills();
const std::vector<MutatorOptionChoice>& create_game_bot_personalities();
// A bot of a default identity (-1: random) at the presets for new bots.
ListenServerBot create_game_new_bot(int identity);
// Cut from the end, or Random bots added.
void create_game_set_bot_count(int count);

// Rows of the rules that pass `shown`, each group once under its heading with its basic rows first, and the height
// they take. Groups go in table order, those with a basic row first if `basic_groups_first`.
using CreateGameRuleFilter = bool (*)(const ListenServerRule& rule, int scope);
int create_game_rules_height(const UiContext& ctx, int scope, CreateGameRuleFilter shown,
                             bool basic_groups_first = false);
void create_game_rule_rows(UiContext& ctx, FormScroll& scroll, int x, int& y, int scope, CreateGameRuleFilter shown,
                           const FormFonts& fonts, bool basic_groups_first = false);
bool create_game_rule_in_play(const ListenServerRule& rule);
// The mutators the game type of `scope` (any in play for the base) has a use for; the base scope also lists the
// rest, grayed.
int create_game_mutators_height(const UiContext& ctx, int scope);
void create_game_mutator_rows(UiContext& ctx, FormScroll& scroll, int x, int& y, int scope, const FormFonts& fonts);
// Takes into the setup what the rows' boxes committed since they were built.
void create_game_flush_edits();
// The strip under a panel's rows, for the row the description state names.
void create_game_draw_description(UiContext& ctx, const UiRect& strip, int font);

// A list entry's edit button: a pencil, gold while `own`, white while `flash`. True when pressed.
bool create_game_edit_button(UiContext& ctx, const UiRect& r, bool own, bool flash, UiId id);
// The dialog frame the entry and bot dialogs share, centered on the canvas. Returns the area under the title band.
UiRect create_game_dialog(const UiContext& ctx, int w_ref, int h_ref, const char* title, UiRect& dialog);
// The note at the top of a dialog's `area`, two lines at most. Returns its height.
int create_game_dialog_note(const UiContext& ctx, const UiRect& area, const std::string& note, int font);

// MAPS and BOTS: a list to add from on the left under its bar, the panel's own list on the right under its heading and
// `count`, each over its footer.
struct CreateGamePanes
{
    UiRect bar;
    UiRect left;
    UiRect right;
    UiRect left_footer;
    UiRect right_footer;
};
CreateGamePanes create_game_panes(const UiContext& ctx, const MenuPanel& panel, const char* heading,
                                  const std::string& count, const MenuFonts& fonts);
// A row of the right list: a cycler `cycler_w` wide against its end (none for 0), and the edit pencil before it.
struct CreateGameEntry
{
    UiRect cycler;
    UiRect edit;
    bool hovered = false;
    bool on_cycler = false;
    bool on_edit = false;

    [[nodiscard]] bool on_row() const
    {
        return hovered && !on_cycler && !on_edit;
    }
};
CreateGameEntry create_game_entry(const UiContext& ctx, const SkinScrollListRow& row, int cycler_w);
enum class CreateGameEntryClick
{
    None,
    Pick,
    Remove,
    Edit,
};
// Hit-test pass: a click on the row picks it, a middle click removes it, a right click edits it.
CreateGameEntryClick create_game_entry_click(const UiContext& ctx, const SkinListRow& r, const CreateGameEntry& entry);
// The entry's number; returns where its text starts.
int create_game_draw_entry_index(const UiContext& ctx, const UiRect& d, int index, int font);
// Scrolls a list so `row` shows whole in `well`.
void create_game_reveal(const UiContext& ctx, const UiRect& well, const UiRect& row, float& scroll);
enum class CreateGameListOp
{
    None,
    Up,
    Down,
    Remove,
    Clear,
};
// The right list's Up, Down, Remove and Clear along `footer`; `movable` gates all but Clear.
CreateGameListOp create_game_list_buttons(UiContext& ctx, const UiRect& footer, int selected, int count, bool movable,
                                          int font, const char* id_name);

class CreateGameRowsPanel final : public ColumnPanel
{
public:
    [[nodiscard]] bool is_open() const override
    {
        return open_;
    }
    void open() override
    {
        open_ = true;
    }
    void close() override
    {
        open_ = false;
    }
    bool back() override
    {
        return false;
    }
    void build(UiContext& ctx, const MenuPanel& panel, const MenuFonts& fonts) override;
    void commit() override;
    void reset();

private:
    void build_options(UiContext& ctx, FormScroll& scroll, int x, int& y, const FormFonts& fonts);
    [[nodiscard]] int options_height(const UiContext& ctx) const;

    bool open_ = false;
    float scroll_ = 0.0f;
    UiId scrolled_to_ = 0;
};

class CreateGameMutatorsPanel final : public ColumnPanel
{
public:
    [[nodiscard]] bool is_open() const override
    {
        return open_;
    }
    void open() override
    {
        open_ = true;
    }
    void close() override
    {
        open_ = false;
    }
    bool back() override
    {
        return false;
    }
    void build(UiContext& ctx, const MenuPanel& panel, const MenuFonts& fonts) override;
    void commit() override;
    void reset();

private:
    bool open_ = false;
    float scroll_ = 0.0f;
    UiId scrolled_to_ = 0;
};

class CreateGameMapsPanel final : public ColumnPanel
{
public:
    [[nodiscard]] bool is_open() const override
    {
        return open_;
    }
    void open() override
    {
        open_ = true;
    }
    void close() override;
    bool back() override
    {
        return false;
    }
    void build(UiContext& ctx, const MenuPanel& panel, const MenuFonts& fonts) override;
    void commit() override;
    bool key(UiContext& ctx, int key) override;
    void hints(const UiContext& ctx, std::vector<MenuHint>& hints) const override;
    [[nodiscard]] bool modal_open() const override
    {
        return edit_ >= 0;
    }
    void build_modal(UiContext& ctx) override;
    bool modal_key(UiContext& ctx, int key) override;
    // The rotation and the list it is filtered from changed under the panel.
    void reset();
    // The Show filter takes the filter of a game type just picked or loaded.
    void follow_game_type(rf::NetGameType game_type);

private:
    void filter();
    void build_list(UiContext& ctx, const UiRect& well, int font);
    void build_rotation(UiContext& ctx, const UiRect& well, int font);
    void draw_card(const UiContext& ctx, const std::string& filename, const UiRect& row, const UiRect& well) const;
    void add(const std::string& filename);
    void move(int to);
    void remove(int index);
    void copy_settings(int entry);
    void paste_settings(int entry);
    void open_edit(int entry);
    [[nodiscard]] rf::NetGameType default_type_for(const std::string& filename) const;
    // Locally installed waypoints; a level without may still get them from FactionFiles when it loads.
    [[nodiscard]] bool has_waypoints(const std::string& filename) const;

    bool open_ = false;
    std::string search_;
    // A game type whose level names it shows, or rf::NG_TYPE_UNK for every map.
    int show_ = rf::NG_TYPE_UNK;
    std::vector<int> shown_;
    float list_scroll_ = 0.0f;
    UiId list_scrolled_to_ = 0;
    int selected_ = -1;
    bool reveal_ = false;
    float rotation_scroll_ = 0.0f;
    UiId rotation_scrolled_to_ = 0;
    // The hover card: the level the cursor rests on, its row, and the list the row is in.
    MenuHoverRest<std::string> hover_;
    std::string hover_pass_;
    UiRect hover_rect_{};
    UiRect hover_pass_rect_{};
    UiRect hover_well_{};
    UiRect hover_pass_well_{};
    // has_waypoints' answers by lowercase filename, for the screen's visit.
    mutable std::unordered_map<std::string, bool> has_waypoints_;
    // The entry dialog, and the settings clipboard for the screen's visit.
    int edit_ = -1;
    float edit_scroll_ = 0.0f;
    UiId edit_scrolled_to_ = 0;
    std::optional<toml::table> clipboard_;
    int pasted_ = -1;
    std::chrono::steady_clock::time_point pasted_at_{};
};

class CreateGameBotsPanel final : public ColumnPanel
{
public:
    [[nodiscard]] bool is_open() const override
    {
        return open_;
    }
    void open() override
    {
        open_ = true;
    }
    void close() override;
    bool back() override
    {
        return false;
    }
    void build(UiContext& ctx, const MenuPanel& panel, const MenuFonts& fonts) override;
    void commit() override {}
    bool key(UiContext& ctx, int key) override;
    void hints(const UiContext& ctx, std::vector<MenuHint>& hints) const override;
    [[nodiscard]] bool modal_open() const override
    {
        return edit_ >= 0;
    }
    void build_modal(UiContext& ctx) override;
    bool modal_key(UiContext& ctx, int key) override;
    void reset();

private:
    void build_pool(UiContext& ctx, const UiRect& well, int font);
    void build_roster(UiContext& ctx, const UiRect& well, int font);
    void add(int identity);
    void move(int to);
    void remove(int index);

    bool open_ = false;
    int selected_ = -1;
    bool reveal_ = false;
    float pool_scroll_ = 0.0f;
    UiId pool_scrolled_to_ = 0;
    float roster_scroll_ = 0.0f;
    UiId roster_scrolled_to_ = 0;
    int edit_ = -1;
    std::string name_edit_;
};
