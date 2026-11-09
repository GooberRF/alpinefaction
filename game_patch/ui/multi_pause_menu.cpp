#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <common/utils/string-utils.h>
#include "multi_pause_menu.h"
#include "menu_shell.h"
#include "reference_data.h"
#include "reference_menu.h"
#include "settings_skin.h"
#include "ui_kit.h"
#include "../graphics/mesh_preview.h"
#include "../misc/misc.h"
#include "../misc/settings_registry.h"
#include "../multi/demo/demo.h"
#include "../rf/entity.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/input.h"
#include "../rf/localize.h"
#include "../rf/multi.h"
#include "../rf/object.h"
#include "../rf/player/player.h"
#include "../rf/ui.h"
#include "../rf/vmesh.h"
#include "../rf/weapon.h"

namespace
{

enum class PauseAction
{
    Player,
    Options,
    Reference,
    Return,
    Quit,
};

struct PauseButton
{
    PauseAction action;
    rf::Key hotkey;
};

// Column order; Quit takes the column's Back slot. Stock's letters stay theirs, so R is Return.
constexpr std::array<PauseButton, 5> buttons{{
    {PauseAction::Player, rf::KEY_P},
    {PauseAction::Options, rf::KEY_O},
    {PauseAction::Reference, rf::KEY_NONE},
    {PauseAction::Return, rf::KEY_R},
    {PauseAction::Quit, rf::KEY_Q},
}};

// The team the 3D view dresses the character for; Neutral shows its own skin.
constexpr std::array<const char*, 3> team_choices{"Neutral", "Red", "Blue"};

UiContext g_ui;
bool g_active = false;
bool g_closing = false;
// 0 = off canvas, 1 = seated. Rises on entry, falls again once the menu is left.
float g_slide = 0.0f;
// REFERENCE was taken: the column slides out, then the Reference screen slides in over the same chrome.
bool g_reference_pending = false;
bool g_player_open = false;
float g_player_slide = 0.0f;
std::array<MenuColumnItem, buttons.size() - 1> g_items;
std::string g_quit_label;
// Taken by the hit-test pass, acted on after it.
std::optional<PauseAction> g_pending;
int g_pending_character = -1;

// The Player panel. The view and team choices last the session.
bool g_portrait_view = false;
int g_team_choice = 0;
std::string g_name;
UiId g_prev_text_focus = 0;
float g_grid_scroll = 0.0f;
UiId g_grid_scrolled_to = 0;
MeshPreview g_preview;
// What g_preview holds, so a character loads once per pick and one that fails is not retried.
std::optional<std::pair<int, int>> g_preview_shown; // character, team
// Where the latest draw pass put the 3D view; empty while it shows none.
UiRect g_preview_rect{};

int selected_character()
{
    const int count = mp_character_count();
    if (!rf::local_player || count <= 0) {
        return -1;
    }
    return std::clamp(rf::local_player->settings.multi_character, 0, count - 1);
}

int preview_team()
{
    return g_team_choice == 0 ? -1 : g_team_choice == 1 ? rf::TEAM_RED : rf::TEAM_BLUE;
}

// The Assault Rifle, the weapon most characters are pictured with.
int shown_weapon()
{
    const int weapon = rf::assault_rifle_weapon_type;
    return weapon >= 0 && weapon < rf::num_weapon_types ? weapon : -1;
}

// The set's relaxed stand for the weapon where it has one, else the weapon-out attack_stand: most sets' plain
// stand is a pistol idle.
std::string stand_anim(const rf::EntityInfo& anims, int weapon)
{
    const auto find = [](const rf::EntityAnimInfo* list, int count, const char* state) -> const rf::EntityAnimInfo* {
        for (int i = 0; list && i < count; ++i) {
            if (string_iequals(list[i].name.c_str(), state)) {
                return &list[i];
            }
        }
        return nullptr;
    };
    const rf::EntityAnimInfo* weapon_anims = weapon >= 0 ? anims.state_weapon_anims[weapon] : nullptr;
    const int num_weapon_anims = weapon >= 0 ? anims.num_state_weapon_anims[weapon] : 0;
    const rf::EntityAnimInfo* anim = find(weapon_anims, num_weapon_anims, "stand");
    if (!anim) {
        anim = find(weapon_anims, num_weapon_anims, "attack_stand");
    }
    if (!anim) {
        anim = find(anims.state_anims, anims.num_state_anims, "attack_stand");
    }
    return anim ? anim->anim_filename.c_str() : std::string{};
}

// The props entity setup (0x004246E0) tries for a held weapon, in its order.
std::vector<MeshPreviewPart> held_weapon(rf::VMesh* mesh, int weapon)
{
    const char* weapon_mesh = weapon_third_person_mesh(weapon);
    if (!*weapon_mesh) {
        return {};
    }
    for (const char* prop_name : {"primary_weapon_1", "secondary_weapon_1", "primary_1", "secondary_1"}) {
        if (const int prop = rf::vmesh_lookup_prop_point(mesh, prop_name); prop >= 0) {
            return {MeshPreviewPart{weapon_mesh, {}, {}, prop, "grip_1"}};
        }
    }
    return {};
}

void show_character(int index, int team)
{
    g_preview_shown = {index, team};
    if (index < 0 || index >= mp_character_count()) {
        g_preview.set_mesh({});
        return;
    }
    rf::MpCharacterInfo& character = rf::mp_characters[index];
    if (character.entity_type < 0 || character.entity_type >= rf::num_entity_types
        || character.anim_entity_type < 0 || character.anim_entity_type >= rf::num_entity_types) {
        g_preview.set_mesh({});
        return;
    }
    rf::EntityInfo& info = rf::entity_types[character.entity_type];
    std::string skin{character.skin_name, strnlen(character.skin_name, sizeof(character.skin_name))};
    if (team >= 0) {
        skin += team == rf::TEAM_BLUE ? "_blue" : "_red";
    }
    // As player_create_entity (0x004A4130) dresses a player, team skins of the "default" skin built on first use.
    const auto dress = [&](rf::VMesh* mesh) {
        // The builder appends its two skins with no check against the table's room.
        const bool room = info.num_alt_skins + 2 <= static_cast<int>(std::size(info.alt_skins));
        if (team >= 0 && room && rf::mp_character_lacks_team_skins(&character, &info)) {
            // It reads only the object's vmesh (+0x80).
            alignas(rf::Object) std::array<std::byte, sizeof(rf::Object)> holder{};
            reinterpret_cast<rf::Object*>(holder.data())->vmesh = mesh;
            rf::mp_character_add_team_skins(&character, reinterpret_cast<rf::Object*>(holder.data()));
        }
        rf::vmesh_set_skin(mesh, info.num_alt_skins, info.alt_skins, skin.c_str());
    };
    const int weapon = shown_weapon();
    g_preview.set_character(info.vmesh_filename.c_str(),
                            stand_anim(rf::entity_types[character.anim_entity_type], weapon), dress,
                            [weapon](rf::VMesh* mesh) { return held_weapon(mesh, weapon); });
}

void render_preview()
{
    if (g_preview_rect.w <= 0 || g_preview_rect.h <= 0) {
        return;
    }
    const std::pair shown{selected_character(), preview_team()};
    if (g_preview_shown != shown) {
        show_character(shown.first, shown.second);
    }
    g_preview.render_in_well(g_preview_rect.w, g_preview_rect.h);
}

void release_preview()
{
    g_preview.release();
    g_preview_shown.reset();
    g_preview_rect = UiRect{};
}

void make_items()
{
    g_items = {
        MenuColumnItem{menu_stock_label(rf::strings::player), false, true},
        MenuColumnItem{menu_stock_label(rf::strings::options), false, false},
        MenuColumnItem{"REFERENCE", false, false},
        MenuColumnItem{menu_stock_label(rf::strings::main_menu_return), false, false},
    };
    g_quit_label = menu_stock_label(rf::strings::quit);
}

// ---------------------------------------------------------------------------
// Player panel
// ---------------------------------------------------------------------------

// Along the bottom of the view. The portrait has no team, so it is inactive there.
void build_team_toggle(UiContext& ctx, const UiRect& bar, const MenuFonts& fonts)
{
    const char* text = "Team";
    const SkinCyclerRects rects = skin_header_cycler_rects(ctx, bar, text, 130.0f, fonts.body, true);
    const int step = skin_labeled_cycler(ctx, rects.label, rects.toggle, text, team_choices[g_team_choice], fonts.body,
                                         !g_portrait_view, ui_id("team_toggle"), skin::txt_hi, skin::txt);
    if (step != 0) {
        const int count = static_cast<int>(team_choices.size());
        g_team_choice = (g_team_choice + step + count) % count;
        ui_play_click();
    }
}

void build_view(UiContext& ctx, const UiRect& r, const MenuFonts& fonts)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const int name_h = rf::gr::get_font_height(fonts.name) + s(14.0f);
    const UiRect view{r.x, r.y, r.w, r.h - name_h};
    const int bar_h = s(skin::widget_h) + s(20.0f);
    const UiRect shown{view.x, view.y, view.w, view.h - bar_h};
    if (ctx.draw) {
        const int character = selected_character();
        int bm = -1;
        UiRect image{shown.x + 1, shown.y + 1, shown.w - 2, shown.h - 2};
        if (g_portrait_view) {
            bm = character >= 0 ? rf::mp_characters[character].image_handle : -1;
            image = UiRect{image.x + s(12.0f), image.y + s(12.0f), image.w - s(24.0f), image.h - s(24.0f)};
        }
        else {
            g_preview_rect = shown;
            bm = g_preview.bitmap();
        }
        skin_preview_well(ctx, view, image, bm, fonts.caption);
        if (const char* caption = mp_character_caption(character)) {
            const SkinText name = skin_fit_text(ctx, caption, fonts.name, r.w);
            rf::gr::set_color(skin::ink_title);
            rf::gr::string_aligned(rf::gr::ALIGN_CENTER, r.x + r.w / 2,
                                   r.y + r.h - name_h + (name_h - rf::gr::get_font_height(name.font)) / 2,
                                   name.text.c_str(), name.font);
        }
    }
    build_team_toggle(ctx, UiRect{view.x, view.y + view.h - bar_h, view.w, bar_h}, fonts);
}

void build_name_row(UiContext& ctx, const UiRect& r, const MenuFonts& fonts)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const UiId id = ui_id("player_name");
    // Away from the field the box shows the name in force, so a rejected edit falls back to it.
    if (!ctx.draw && ctx.focused_text_id() != id && rf::local_player) {
        g_name = rf::local_player->name.c_str();
    }
    // A demo's player is not the one these settings dress.
    const bool enabled = rf::local_player != nullptr && !demo_playback_active();
    const char* label = rf::strings::player_name;
    const int label_w = rf::gr::get_string_size(label, fonts.body).first + s(20.0f);
    if (ctx.draw) {
        rf::gr::set_color(skin::ink_title);
        rf::gr::string(r.x, r.y + (r.h - rf::gr::get_font_height(fonts.body)) / 2, label, fonts.body);
    }
    const char* warn = enabled ? player_name_warning(g_name) : nullptr;
    const UiRect field{r.x + label_w, r.y, r.w - label_w, r.h};
    skin_field(ctx, field, id, g_name, player_name_field_len, player_name_char_allowed, "Player", fonts.body, enabled,
               warn != nullptr, nullptr);
    // Committed when the box lets the focus go, as the settings menu's row is: the engine tells the server.
    if (!ctx.draw && enabled && ctx.focused_text_id() == id && g_prev_text_focus != id) {
        ctx.set_text_commit_callback([](bool commit) {
            const SettingRow* row = settings_find("player_name");
            if (commit && row && !player_name_warning(g_name)) {
                setting_set_from_string(*row, g_name);
            }
        });
    }
    if (ctx.draw && warn) {
        rf::gr::set_color(skin::warn);
        rf::gr::string(field.x, field.y + field.h + s(4.0f), warn, fonts.caption);
    }
}

void build_grid(UiContext& ctx, const UiRect& well, int tile_font)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    if (ctx.draw) {
        skin_slot(ctx, well.x, well.y, well.w, well.h);
    }
    const SkinPickerGrid grid{
        .count = mp_character_count(),
        .selected = selected_character(),
        .tile_w = skin::portrait_tile,
        .tile_h = skin::portrait_tile,
        .bitmap = [](int i) { return rf::mp_characters[i].image_handle; },
        .caption = mp_character_caption,
        .caption_line = true,
        .cut_end = true,
        .font = tile_font,
        .id_name = "character",
        // A demo's player is not the one these settings dress.
        .enabled = !demo_playback_active(),
    };
    const UiRect view{well.x + s(4.0f), well.y + s(4.0f), well.w - s(8.0f), well.h - s(8.0f)};
    const int picked = skin_picker_grid(ctx, view, grid, g_grid_scroll, g_grid_scrolled_to);
    if (picked >= 0) {
        g_pending_character = picked;
    }
}

// Right-aligned on the header line, as the settings menu's Show advanced is.
void build_view_toggle(UiContext& ctx, const UiRect& line, const MenuFonts& fonts)
{
    const char* text = "View";
    const SkinCyclerRects rects = skin_header_cycler_rects(ctx, line, text, 110.0f, fonts.body);
    if (skin_labeled_cycler(ctx, rects.label, rects.toggle, text, g_portrait_view ? "Portrait" : "3D", fonts.body,
                            true, ui_id("view_toggle"), skin::ink_title, skin::ink_status) != 0) {
        g_portrait_view = !g_portrait_view;
        ui_play_toggle(!g_portrait_view);
    }
}

void build_player_panel(UiContext& ctx, const MenuPanel& panel, const MenuFonts& fonts)
{
    const auto s = [&](float v) { return ctx.scaled(v); };
    const UiRect& header = panel.header;
    const UiRect& body = panel.body;
    if (ctx.draw) {
        menu_draw_panel_heading(ctx, panel, menu_stock_label(rf::strings::player).c_str(), fonts.heading);
    }
    build_view_toggle(ctx, header, fonts);

    // The character stands tall, so the view takes a tall column and the picks fill the width beside it.
    const int gap = s(24.0f);
    const int view_w = std::clamp(body.w * 9 / 25, s(260.0f), s(480.0f));
    build_view(ctx, UiRect{body.x, body.y, view_w, body.h}, fonts);

    const int right_x = body.x + view_w + gap;
    const int right_w = body.x + body.w - right_x;
    const int field_h = s(44.0f);
    build_name_row(ctx, UiRect{right_x, body.y, right_w, field_h}, fonts);
    const int grid_y = body.y + field_h + s(34.0f);
    build_grid(ctx, UiRect{right_x, grid_y, right_w, body.y + body.h - grid_y}, menu_font(false, 15.0f, ctx.scale()));
}

// ---------------------------------------------------------------------------
// Screen
// ---------------------------------------------------------------------------

void build_screen(UiContext& ctx)
{
    const MenuFrame frame = menu_frame(ctx, g_slide);
    const MenuLayout& lay = frame.lay;
    const int panel_dx = frame.enter_dx + static_cast<int>((1.0f - g_player_slide) * static_cast<float>(frame.slide_w));

    if (ctx.draw) {
        g_preview_rect = UiRect{};
        menu_draw_title_bar(ctx, lay.panel_x + frame.enter_dx, lay.panel_r + frame.enter_dx,
                            lay.rail_right + frame.enter_dx, frame.canvas_y,
                            menu_stock_label(rf::strings::multiplayer).c_str(), frame.fonts.title);
    }

    g_items[0].selected = g_player_open;
    const int clicked = menu_build_column(ctx, lay, lay.column_x + frame.col_dx, frame.top, g_items,
                                          g_quit_label.c_str(), frame.fonts.column, "pause");
    if (clicked >= 0 && !ctx.draw) {
        g_pending = buttons[clicked].action;
    }

    if (g_player_slide > 0.0f) {
        const MenuPanel panel = menu_panel(ctx, lay, panel_dx, frame.top, frame.canvas_y);
        if (ctx.draw) {
            menu_draw_panel(ctx, panel);
            menu_draw_connector(ctx, panel, frame.top + menu_button_h(ctx) / 2);
        }
        // A panel on its way out is only painted.
        if (ctx.draw || g_player_open) {
            build_player_panel(ctx, panel, frame.fonts);
        }
    }

    // Stock draws its side bars over its menus.
    if (ctx.draw) {
        rf::gr::reset_clip();
        rf::ui::menu_draw_side_bars();
    }
}

void run_action(PauseAction action)
{
    switch (action) {
        case PauseAction::Player:
            g_player_open = !g_player_open;
            // Shown afresh, so the character turns from facing the camera each time the panel opens.
            g_preview_shown.reset();
            ui_play_click();
            break;
        case PauseAction::Options:
            rf::ui::mainmenu_button_action(rf::ui::MAIN_MENU_BUTTON_OPTIONS);
            g_closing = true;
            break;
        case PauseAction::Reference:
            g_reference_pending = true;
            ui_play_click();
            break;
        case PauseAction::Return:
            rf::ui::mainmenu_button_action(rf::ui::MAIN_MENU_BUTTON_RETURN);
            g_closing = true;
            break;
        case PauseAction::Quit:
            // Stock's confirmation; its Yes leaves the game, which closes this state without waiting.
            rf::ui::mainmenu_button_action(rf::ui::MAIN_MENU_BUTTON_QUIT);
            break;
    }
}

void commit_pending()
{
    const int character = std::exchange(g_pending_character, -1);
    if (character >= 0 && character != selected_character() && !demo_playback_active()) {
        if (const SettingRow* row = settings_find("multi_character")) {
            setting_set_from_string(*row, std::to_string(character));
            ui_play_click();
        }
    }
    if (const std::optional<PauseAction> action = std::exchange(g_pending, std::nullopt)) {
        run_action(*action);
    }
}

void __cdecl key_callback(int key)
{
    if (key == rf::KEY_NONE) {
        return;
    }
    // A focused text box owns everything; the context replays it at the start of the next hit-test pass.
    if (g_ui.focused_text_id() != 0) {
        g_ui.push_key(static_cast<rf::Key>(key));
        return;
    }
    const int code = key & rf::KEY_MASK;
    // ESC returns to the game, as stock's does, once the Player panel is shut.
    if (code == rf::KEY_ESC) {
        g_pending = g_player_open ? PauseAction::Player : PauseAction::Return;
        return;
    }
    // Hotkeys need no modifier held, as stock's do.
    if (key == code) {
        for (const PauseButton& button : buttons) {
            if (code == button.hotkey) {
                g_pending = button.action;
                return;
            }
        }
    }
    g_ui.handle_nav_key(static_cast<rf::Key>(key));
}

void handle_input()
{
    menu_run_hit_test(g_ui, key_callback, build_screen);
    g_prev_text_focus = g_ui.focused_text_id();
    commit_pending();
}

} // namespace

void multi_pause_menu_on_main_menu_init()
{
    const bool active = !legacy_menus_enabled() && rf::is_multi && rf::ui::mainmenu_game_beneath;
    if (!active) {
        if (g_active) {
            multi_pause_menu_release();
        }
        return;
    }
    g_active = true;
    g_closing = false;
    g_slide = 0.0f;
    g_reference_pending = false;
    g_player_open = false;
    g_player_slide = 0.0f;
    g_pending.reset();
    g_pending_character = -1;
    g_prev_text_focus = 0;
    g_grid_scrolled_to = 0;
    release_preview();
    reference_menu_release();
    make_items();
    menu_ui_reset(g_ui);
    // Stock's init has played the slide-in.
}

bool multi_pause_menu_is_active()
{
    return g_active;
}

void multi_pause_menu_do_frame(bool paused)
{
    if (reference_menu_host_frame(g_reference_pending, g_slide, g_closing, paused)) {
        // One turning preview at a time.
        release_preview();
        return;
    }
    const float step = menu_slide_step();
    g_player_slide = g_player_open ? std::min(1.0f, g_player_slide + step) : std::max(0.0f, g_player_slide - step);

    // paused = 1 means a stock popup owns input this frame; it renders after us and on top.
    if (!paused && !g_closing && !g_reference_pending) {
        handle_input();
    }

    // Before any of the frame's 2D: the preview switches the render target.
    render_preview();
    menu_run_draw(g_ui, build_screen);
}

bool multi_pause_menu_is_closed()
{
    // A change the menu did not start (a level change, a kick, Quit confirmed) does not wait for the slide.
    if (g_closing && g_slide > 0.0f) {
        return false;
    }
    multi_pause_menu_release();
    return true;
}

void multi_pause_menu_release()
{
    g_active = false;
    // A forced close drops a half-typed name.
    g_ui.release_text_focus(false);
    release_preview();
    reference_menu_release();
}
