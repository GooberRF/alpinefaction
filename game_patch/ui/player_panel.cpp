#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <common/utils/string-utils.h>
#include "player_panel.h"
#include "menu_shell.h"
#include "reference_data.h"
#include "settings_skin.h"
#include "ui_kit.h"
#include "../misc/misc.h"
#include "../misc/settings_registry.h"
#include "../multi/demo/demo.h"
#include "../rf/entity.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/localize.h"
#include "../rf/multi.h"
#include "../rf/object.h"
#include "../rf/player/player.h"
#include "../rf/vmesh.h"
#include "../rf/weapon.h"

namespace
{

// The team the 3D view dresses the character for; Neutral shows its own skin.
constexpr std::array<const char*, 3> team_choices{"Neutral", "Red", "Blue"};

// The view and team choices last the session, whichever menu shows the panel.
bool g_portrait_view = false;
int g_team_choice = 0;
bool g_seat_requested = false;

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

// Inactive in the portrait view, which has no team.
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

} // namespace

void player_panel_request_seat(bool seat)
{
    g_seat_requested = seat;
}

bool player_panel_take_seat_request()
{
    return std::exchange(g_seat_requested, false);
}

void PlayerPanel::set_open(bool open)
{
    open_ = open;
    // Shown afresh, so the character turns from facing the camera each time the panel opens.
    preview_shown_.reset();
}

void PlayerPanel::reset()
{
    open_ = false;
    pending_character_ = -1;
    prev_text_focus_ = 0;
    grid_scrolled_to_ = 0;
    release_preview();
}

void PlayerPanel::show_character(int index, int team)
{
    preview_shown_ = {index, team};
    if (index < 0 || index >= mp_character_count()) {
        preview_.set_mesh({});
        return;
    }
    rf::MpCharacterInfo& character = rf::mp_characters[index];
    if (character.entity_type < 0 || character.entity_type >= rf::num_entity_types
        || character.anim_entity_type < 0 || character.anim_entity_type >= rf::num_entity_types) {
        preview_.set_mesh({});
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
    preview_.set_character(info.vmesh_filename.c_str(),
                           stand_anim(rf::entity_types[character.anim_entity_type], weapon), dress,
                           [weapon](rf::VMesh* mesh) { return held_weapon(mesh, weapon); });
}

void PlayerPanel::render_preview()
{
    if (preview_rect_.w > 0 && preview_rect_.h > 0) {
        const std::pair shown{mp_character_selected(), preview_team()};
        if (preview_shown_ != shown) {
            show_character(shown.first, shown.second);
        }
        preview_.render_in_well(preview_rect_.w, preview_rect_.h);
    }
    preview_rect_ = UiRect{};
}

void PlayerPanel::release_preview()
{
    preview_.release();
    preview_shown_.reset();
    preview_rect_ = UiRect{};
}

void PlayerPanel::build_view(UiContext& ctx, const UiRect& r, const MenuFonts& fonts)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const int name_h = rf::gr::get_font_height(fonts.name) + s(14.0f);
    const UiRect view{r.x, r.y, r.w, r.h - name_h};
    const int bar_h = s(skin::widget_h) + s(20.0f);
    const UiRect shown{view.x, view.y, view.w, view.h - bar_h};
    if (ctx.draw) {
        const int character = mp_character_selected();
        int bm = -1;
        UiRect image{shown.x + 1, shown.y + 1, shown.w - 2, shown.h - 2};
        if (g_portrait_view) {
            bm = character >= 0 ? rf::mp_characters[character].image_handle : -1;
            image = UiRect{image.x + s(12.0f), image.y + s(12.0f), image.w - s(24.0f), image.h - s(24.0f)};
        }
        else {
            preview_rect_ = shown;
            bm = preview_.bitmap();
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

void PlayerPanel::build_name_row(UiContext& ctx, const UiRect& r, const MenuFonts& fonts)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const UiId id = ui_id("player_name");
    // Away from the field the box shows the name in force, so a rejected edit falls back to it.
    if (!ctx.draw && ctx.focused_text_id() != id && rf::local_player) {
        name_ = rf::local_player->name.c_str();
    }
    // A demo's player is not the one these settings dress.
    const bool enabled = rf::local_player != nullptr && !demo_playback_active();
    const char* label = rf::strings::player_name;
    const int label_w = rf::gr::get_string_size(label, fonts.body).first + s(20.0f);
    if (ctx.draw) {
        rf::gr::set_color(skin::ink_title);
        rf::gr::string(r.x, r.y + (r.h - rf::gr::get_font_height(fonts.body)) / 2, label, fonts.body);
    }
    const char* warn = enabled ? player_name_warning(name_) : nullptr;
    const UiRect field{r.x + label_w, r.y, r.w - label_w, r.h};
    skin_field(ctx, field, id, name_, player_name_field_len, player_name_char_allowed, "Player", fonts.body, enabled,
               warn != nullptr, nullptr);
    // Committed when the box lets the focus go, as the settings menu's row is: the engine tells the server.
    if (!ctx.draw && enabled && ctx.focused_text_id() == id && prev_text_focus_ != id) {
        ctx.set_text_commit_callback([this](bool commit) {
            const SettingRow* row = settings_find("player_name");
            if (commit && row && !player_name_warning(name_)) {
                setting_set_from_string(*row, name_);
            }
        });
    }
    if (ctx.draw && warn) {
        rf::gr::set_color(skin::warn);
        rf::gr::string(field.x, field.y + field.h + s(4.0f), warn, fonts.caption);
    }
}

void PlayerPanel::build_grid(UiContext& ctx, const UiRect& well, int tile_font)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    if (ctx.draw) {
        skin_slot(ctx, well.x, well.y, well.w, well.h);
    }
    const SkinPickerGrid grid{
        .count = mp_character_count(),
        .selected = mp_character_selected(),
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
    const int picked = skin_picker_grid(ctx, view, grid, grid_scroll_, grid_scrolled_to_);
    if (picked >= 0) {
        pending_character_ = picked;
    }
}

void PlayerPanel::build(UiContext& ctx, const MenuPanel& panel, const MenuFonts& fonts)
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

void PlayerPanel::commit()
{
    const int character = std::exchange(pending_character_, -1);
    if (character >= 0 && character != mp_character_selected() && !demo_playback_active()) {
        if (const SettingRow* row = settings_find("multi_character")) {
            setting_set_from_string(*row, std::to_string(character));
            ui_play_click();
        }
    }
}
