#pragma once

#include <chrono>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "ui_kit.h"
#include "../rf/gr/gr.h"

// The frame Alpine's menu screens share: the bar down the left edge, the button column beside it, the title bar, the
// path strip along the top and the hint bar along the bottom, all sized against a 1920x1080 reference.

inline constexpr float menu_ref_w = 1920.0f;
inline constexpr float menu_ref_h = 1080.0f;
// The path strip along the screen's top edge and the hint bar along its bottom edge. The canvas below the path strip
// is moved down by its height.
inline constexpr float menu_strip_h = 40.0f;
inline constexpr float menu_column_y = 130.0f + menu_strip_h;
// The gap between the column and a panel, which a connector bridges.
inline constexpr float menu_connector_w = 48.0f;
// Small button slots down the column, which a panel spans whatever the column's button size. The
// buttons then BACK fill them from the top and the rest stay empty, as on stock.
inline constexpr int menu_panel_slots = 11;
// Large slots, stock's button height, to the same span.
inline constexpr int menu_large_slots = 6;

enum class MenuButtonSize
{
    Small,
    Large,
};

// A panel's header line, and where its body starts below the header's ridge.
inline constexpr float menu_panel_header_y = 158.0f + menu_strip_h;
inline constexpr float menu_panel_header_h = 54.0f;
inline constexpr float menu_panel_body_y = 218.0f + menu_strip_h;
// A panel's footer line, measured up from the bottom of its steel field.
inline constexpr float menu_panel_footer_inset = 52.0f;
inline constexpr float menu_panel_footer_h = 40.0f;

// Horizontal anchors in screen pixels.
struct MenuLayout
{
    int column_x = 0;      // the buttons' faces, their plates showing between them and the bar
    int column_hidden = 0; // how far each button reaches left of it, out under the bar
    int column_w = 0;      // button width from column_x, end cap included
    int panel_x = 0;       // seated
    int panel_r = 0;
};

// The panel beside the column: its steel frame and the recessed field inside it.
struct MenuPanel
{
    UiRect frame;
    UiRect inset;
    // Where content goes, clear of the frame's channel and its right edge.
    int content_x = 0;
    int content_r = 0;
    // The header line, and the body below its ridge down to the field's bottom margin.
    UiRect header;
    UiRect body;
};

struct MenuFonts
{
    int column;
    int heading;
    int name;
    int body;
    int caption;
};

// Where a menu screen's chrome sits this frame, for `slide` (0 = off canvas, 1 = seated) of its entry.
struct MenuFrame
{
    int canvas_y = 0;
    MenuLayout lay;
    int slide_w = 0;  // how far a panel travels off to the right
    int enter_dx = 0; // the title bar and panel, coming in from the right
    int col_dx = 0;   // the column, coming in from the left
    int top = 0;      // the column's first slot
    MenuFonts fonts{};
};

struct MenuColumnItem
{
    std::string label;
    bool selected = false;
    // Off for a button that acts rather than opening a panel.
    bool arrow = true;
    bool enabled = true;
};

// A stock string as a column label: upper case, its line breaks folded to single spaces for the column to
// break itself.
std::string menu_stock_label(const char* text);
float menu_screen_scale();
// Also sets the context's scale.
MenuFrame menu_frame(UiContext& ctx, float slide);
int menu_font(bool bold, float ref_px, float scale);
MenuLayout menu_layout(const UiContext& ctx);
// A small slot's height.
int menu_button_h(const UiContext& ctx);
// The top of slot `slot` down the column, relative to the column's top.
int menu_slot_y(const UiContext& ctx, MenuButtonSize size, int slot);
// The middle of slot `slot`, where a connector meets its button.
int menu_slot_mid_y(const UiContext& ctx, MenuButtonSize size, int slot);
// This frame's share of a 0..1 slide. Every slide runs at constant speed for as long as the stock
// menus' do.
float menu_slide_step();
// Returns the free stretch of the title bar after the title.
UiRect menu_draw_title_bar(const UiContext& ctx, int x, int right, int canvas_y, const char* title);
// `dx` slides the panel off to the right; `top` is the column's first slot.
MenuPanel menu_panel(const UiContext& ctx, const MenuLayout& lay, int dx, int top, int canvas_y);
void menu_draw_panel(const UiContext& ctx, const MenuPanel& panel);
void menu_draw_panel_ridge(const UiContext& ctx, const MenuPanel& panel);
// The panel's heading on its header line, and the ridge under it.
void menu_draw_panel_heading(const UiContext& ctx, const MenuPanel& panel, const char* text, int font);
// Bridges the gap from the column button whose slot is centered on `mid_y`.
void menu_draw_connector(const UiContext& ctx, const MenuPanel& panel, int mid_y);
// The whole screen dimmed behind a dialog.
void menu_draw_modal_dim();
// The main menu state's crumb: the title menu's, or the pause menu's over a game.
std::string menu_root_crumb();
// The menus above the screen drawing the frame, root first: the menu states beneath the one it runs in, then `hosts`,
// which stand in for that state: the screens handing off to the screen within it.
std::vector<std::string> menu_crumbs(std::span<const std::string> hosts);
// The menu state the screen drawing the frame runs in, as an index into the state stack; -1 over the game.
int menu_own_state();
// Where step `step` of a path menu_crumbs begins leads back to: the menu state at index `state` of the state stack,
// and within it the screen `host` hand-offs deep.
struct MenuCrumbTarget
{
    int state = -1;
    std::size_t host = 0;
};
MenuCrumbTarget menu_crumb_target(std::size_t step);
// The path strip's player leads to Multi > Player: on the title menus, or over a multiplayer game.
bool menu_player_reachable();

// The hint bar's keys: what each does on the screen. The arrow pairs are drawn, as the menu fonts may lack them.
enum class MenuHintArrows
{
    None,
    UpDown,
    LeftRight,
};
struct MenuHint
{
    std::string key;
    std::string label;
    MenuHintArrows arrows = MenuHintArrows::None;
};
// The keys every menu screen shares, ahead of its own: ESC as `esc` unless it is empty, or Close while a dropdown is
// open, then with `nav` Move, Adjust for a focused Left / Right widget and ENTER Select for a focus still on screen or
// an open dropdown. While a text box takes the typing, only ESC Cancel or Done and ENTER Done.
void menu_add_nav_hints(const UiContext& ctx, std::vector<MenuHint>& hints, std::string esc, bool nav = true);
// What of the path strip takes clicks: the first `steps` of its path, and the player's portrait and name.
struct MenuStripLinks
{
    std::size_t steps = 0;
    bool player = false;
};
// A click on the player, from menu_strip_click.
inline constexpr int menu_strip_player = -2;
// Over a screen's column and panels: the left bar, the path strip with `path` (menu_crumbs, then the screen's own
// steps) and the player's portrait and name, and the hint bar with `hints`, Esc's last, and what is running.
void menu_draw_frame(const UiContext& ctx, std::span<const std::string> path, std::span<const MenuHint> hints,
                     const MenuStripLinks& links = {});
// After a hit-test pass, which of `links` its click took: the step's index in `path`, menu_strip_player, or -1.
int menu_strip_click(const UiContext& ctx, std::span<const std::string> path, const MenuStripLinks& links);
// Where menu_draw_frame puts what is running; empty when nothing is.
UiRect menu_strip_context_rect(const UiContext& ctx);
// The path strip's height in screen pixels while menu_draw_frame draws it, else 0.
int menu_strip_drawn_h();
// `bm` in a slot filling `r`, square from the top middle of the picture, where a portrait's face is. Returns false,
// leaving the slot empty, when `bm` has no picture.
bool menu_draw_portrait(const UiContext& ctx, const UiRect& r, int bm);
// A dialog `w` x `h` screen pixels, centered on the screen. The draw pass dims the screen behind it and paints it.
struct MenuDialog
{
    UiRect frame;
    UiRect band; // the title's
};
MenuDialog menu_dialog(const UiContext& ctx, int w, int h, const char* title, int title_font);

// A hover card waits this long for the cursor to rest on what it shows.
inline constexpr std::chrono::milliseconds menu_card_delay{250};
// What the cursor rests on, and since when.
template<typename Key>
class MenuHoverRest
{
public:
    explicit MenuHoverRest(Key none = {}, std::chrono::milliseconds delay = menu_card_delay) :
        none_{none}, key_{none}, delay_{delay}
    {
    }
    // A change restarts the wait.
    void track(const Key& key)
    {
        if (key != key_) {
            key_ = key;
            since_ = std::chrono::steady_clock::now();
        }
    }
    void clear()
    {
        key_ = none_;
    }
    [[nodiscard]] const Key& key() const
    {
        return key_;
    }
    [[nodiscard]] bool rested() const
    {
        return key_ != none_ && std::chrono::steady_clock::now() - since_ >= delay_;
    }

private:
    Key none_;
    Key key_;
    std::chrono::milliseconds delay_;
    std::chrono::steady_clock::time_point since_{};
};
// A hover card's width within `bounds` (`ref_w` at the reference size), its padding, and the width inside the padding.
struct MenuCard
{
    int w = 0;
    int pad = 0;
    int inner = 0;
};
MenuCard menu_card(const UiContext& ctx, const UiRect& bounds, float ref_w = 560.0f);
// Where a card `h` high goes: under `row`, else over it, from `anchor_x`; where neither fits, beside `anchor_x` on
// the side with room, else the roomier side. Kept inside `bounds`.
UiRect menu_card_place(const UiContext& ctx, const MenuCard& card, int h, const UiRect& row, int anchor_x,
                       const UiRect& bounds);
void menu_draw_card(const UiContext& ctx, const UiRect& r);
// A level's preview with a caption band `cap_h` high along its bottom: `level` from the left and `type` against the
// right, each `pad` in. A preview no taller than the band is the band alone.
struct MenuLevelCaption
{
    std::string_view level;
    std::string_view type;
    int font = 0;
    int cap_h = 0;
    int pad = 0;
    rf::Color level_color;
    rf::Color type_color;
};
void menu_draw_level_preview(const std::string& level_filename, const UiRect& r, const MenuLevelCaption& caption);
// `items` then BACK, one slot each from `y`. Returns the index clicked, `items.size()` for BACK, or -1.
int menu_build_column(UiContext& ctx, const MenuLayout& lay, int x, int y, std::span<const MenuColumnItem> items,
                      const char* back_label, int font, const char* id_name,
                      MenuButtonSize size = MenuButtonSize::Small);

void menu_ui_reset(UiContext& ctx);
// A frame's hit-test pass: the keys go to `key_callback`, the mouse to the context, then `build` runs. A text box
// lets go once the keys have opened the console, before they reach it. A null `key_callback` leaves the keys to a
// host that feeds them itself.
void menu_run_hit_test(UiContext& ctx, void (*key_callback)(int), const std::function<void(UiContext&)>& build);
// A frame's draw pass, over the stock menu background unless `backdrop` is false, popups on top.
void menu_run_draw(UiContext& ctx, const std::function<void(UiContext&)>& build, bool backdrop = true);
