#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <common/utils/string-utils.h>
#include "reference_menu.h"
#include "menu_shell.h"
#include "reference_data.h"
#include "settings_skin.h"
#include "ui_kit.h"
#include "../graphics/mesh_preview.h"
#include "../multi/vehicles/vehicle.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/input.h"
#include "../rf/localize.h"
#include "../rf/sound/sound.h"
#include "../rf/ui.h"
#include "../sound/sound.h"

namespace
{

constexpr std::array<ReferenceArea, reference_area_count> area_order{
    ReferenceArea::weapons,
    ReferenceArea::vehicles,
    ReferenceArea::gametypes,
    ReferenceArea::powerups,
};
constexpr std::array<rf::Key, reference_area_count> area_hotkeys{rf::KEY_W, rf::KEY_V, rf::KEY_G, rf::KEY_P};

using Fonts = MenuFonts;

UiContext g_ui;
bool g_active = false;
bool g_closing = false;
// 0 = off canvas, 1 = seated. Rises on entry, falls again once Back/ESC is taken.
float g_slide = 0.0f;
// The panel's slide out and back in on an area change; the change lands at the turn.
float g_switch = 0.0f;
std::optional<ReferenceArea> g_switch_to;
ReferenceArea g_area = ReferenceArea::weapons;
// Per area, kept across visits within a session.
std::array<int, reference_area_count> g_selected{};
std::array<float, reference_area_count> g_list_scroll{};
float g_detail_scroll = 0.0f;
UiId g_list_scrolled_to = 0;

// Taken by the hit-test pass, acted on after it.
std::optional<int> g_pending_column;
int g_pending_row = -1;

MeshPreview g_preview;
// The entry g_preview holds, so its mesh loads once per selection and one that fails is not retried.
const ReferenceEntry* g_preview_entry = nullptr;
// Where the panel shows the preview, as the latest pass laid it out; empty when the entry has none.
UiRect g_preview_rect{};

int area_index(ReferenceArea area)
{
    return static_cast<int>(area);
}

const std::vector<ReferenceEntry>& current_entries()
{
    return reference_area_entries(g_area);
}

const ReferenceEntry* current_entry()
{
    const std::vector<ReferenceEntry>& entries = current_entries();
    const int sel = g_selected[area_index(g_area)];
    return sel >= 0 && sel < static_cast<int>(entries.size()) ? &entries[sel] : nullptr;
}

void request_close()
{
    g_closing = true;
}

void show_area(ReferenceArea area)
{
    if (g_switch_to) {
        // Picking the panel's own area again turns a slide-out around.
        if (area == g_area) {
            g_switch_to.reset();
        }
        else {
            g_switch_to = area;
        }
        return;
    }
    if (area != g_area) {
        g_switch_to = area;
    }
}

void select_row(int row)
{
    const int count = static_cast<int>(current_entries().size());
    if (row < 0 || row >= count) {
        return;
    }
    int& selected = g_selected[area_index(g_area)];
    if (row != selected) {
        selected = row;
        g_detail_scroll = 0.0f;
    }
}

// ---------------------------------------------------------------------------
// Detail pane
// ---------------------------------------------------------------------------

// A description is plain text: a blank line starts a paragraph, a single line break is a space.
std::vector<std::string> split_paragraphs(std::string_view text)
{
    std::vector<std::string> paragraphs;
    std::string current;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        const std::string_view line = trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.empty()) {
            if (!current.empty()) {
                paragraphs.push_back(std::move(current));
                current.clear();
            }
            continue;
        }
        if (!current.empty()) {
            current += ' ';
        }
        current += line;
    }
    if (!current.empty()) {
        paragraphs.push_back(std::move(current));
    }
    return paragraphs;
}

struct DetailLine
{
    std::string text;
    int x = 0;
    int y = 0;
    int font = 0;
    rf::Color color{};
    bool rule = false; // a divider `x`..`x + w`, drawn instead of text
    int w = 0;
};

// The scrolling half of the pane, relative to its top-left. Wrapping every word is too slow to redo
// twice a frame, so it is kept until the entry, the width or the fonts change.
struct DetailLayout
{
    const ReferenceEntry* entry = nullptr;
    int width = 0;
    int body_font = -1;
    std::vector<DetailLine> lines;
    int height = 0;
};
DetailLayout g_detail_layout;

const DetailLayout& layout_detail(const UiContext& ctx, const ReferenceEntry& entry, int w, const Fonts& fonts)
{
    DetailLayout& lay = g_detail_layout;
    if (lay.entry == &entry && lay.width == w && lay.body_font == fonts.body) {
        return lay;
    }
    lay = DetailLayout{&entry, w, fonts.body, {}, 0};
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const auto push = [&](std::string text, int x, int y, int font, rf::Color color) {
        lay.lines.push_back(DetailLine{std::move(text), x, y, font, color});
    };
    const int body_h = rf::gr::get_font_height(fonts.body);
    const int caption_h = rf::gr::get_font_height(fonts.caption);

    int y = 0;
    for (const std::string& line : ui_wrap_text(entry.name, w, fonts.name)) {
        push(line, 0, y, fonts.name, skin::txt_hi);
        y += rf::gr::get_font_height(fonts.name);
    }
    if (!entry.subtitle.empty()) {
        y += s(2.0f);
        for (const std::string& line : ui_wrap_text(entry.subtitle, w, fonts.caption)) {
            push(line, 0, y, fonts.caption, skin::txt_hint);
            y += caption_h;
        }
    }
    y += s(16.0f);

    const bool has_mp = std::ranges::any_of(entry.stats, [](const ReferenceStat& st) { return !st.mp_value.empty(); });
    const int col_gap = s(12.0f);
    // As wide as the longest label, within limits that leave the values their room.
    int widest_label = 0;
    for (const ReferenceStat& st : entry.stats) {
        widest_label = std::max(widest_label, rf::gr::get_string_size(st.label, fonts.body).first);
    }
    const int label_w = std::clamp(widest_label, w / 4, w * 2 / 5);
    const int value_x = label_w + col_gap;
    const int value_w = has_mp ? (w - value_x - col_gap) / 2 : w - value_x;
    const int mp_x = value_x + value_w + col_gap;
    const int row_gap = s(5.0f);
    if (!entry.stats.empty() && has_mp) {
        const bool roomy = rf::gr::get_string_size("SINGLE PLAYER", fonts.caption).first <= value_w;
        push(roomy ? "SINGLE PLAYER" : "SP", value_x, y, fonts.caption, skin::badge_sp);
        push(roomy ? "MULTIPLAYER" : "MP", mp_x, y, fonts.caption, skin::badge_mp);
        y += caption_h + row_gap;
    }
    else if (!entry.stats.empty() && entry.multiplayer_only) {
        push("MULTIPLAYER", value_x, y, fonts.caption, skin::badge_mp);
        y += caption_h + row_gap;
    }
    for (const ReferenceStat& st : entry.stats) {
        // A value both modes share fills both columns; only a multiplayer difference takes the MP colour.
        const bool mp_differs = !st.mp_value.empty();
        const std::vector<std::string> label = ui_wrap_text(st.label, label_w, fonts.body);
        const std::vector<std::string> value = ui_wrap_text(st.value, value_w, fonts.body);
        const std::vector<std::string> mp =
            has_mp ? ui_wrap_text(mp_differs ? st.mp_value : st.value, value_w, fonts.body)
                   : std::vector<std::string>{};
        const rf::Color& mp_color = mp_differs ? skin::badge_mp : skin::txt_hi;
        for (std::size_t i = 0; i < label.size(); ++i) {
            push(label[i], 0, y + static_cast<int>(i) * body_h, fonts.body, skin::txt_hint);
        }
        for (std::size_t i = 0; i < value.size(); ++i) {
            push(value[i], value_x, y + static_cast<int>(i) * body_h, fonts.body, skin::txt_hi);
        }
        for (std::size_t i = 0; i < mp.size(); ++i) {
            push(mp[i], mp_x, y + static_cast<int>(i) * body_h, fonts.body, mp_color);
        }
        const std::size_t rows = std::max({label.size(), value.size(), mp.size(), std::size_t{1}});
        y += static_cast<int>(rows) * body_h + row_gap;
    }

    const std::vector<std::string> paragraphs = split_paragraphs(entry.description);
    if (!paragraphs.empty()) {
        y += s(12.0f);
        lay.lines.push_back(DetailLine{{}, 0, y, 0, skin::border_dim, true, w});
        y += s(16.0f);
    }
    for (std::size_t p = 0; p < paragraphs.size(); ++p) {
        if (p > 0) {
            y += body_h / 2;
        }
        for (const std::string& line : ui_wrap_text(paragraphs[p], w, fonts.body)) {
            push(line, 0, y, fonts.body, skin::txt);
            y += body_h;
        }
    }
    lay.height = y + s(8.0f);
    return lay;
}

void build_detail(UiContext& ctx, const UiRect& pane, const Fonts& fonts)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    if (ctx.draw) {
        skin_slot(ctx, pane.x, pane.y, pane.w, pane.h);
    }
    const ReferenceEntry* entry = current_entry();
    // The hit-test pass alone records where the preview goes; the next frame renders it at that size.
    if (!ctx.draw) {
        g_preview_rect = UiRect{};
    }
    if (!entry) {
        if (ctx.draw) {
            rf::gr::set_color(skin::txt_hint);
            rf::gr::string_aligned(rf::gr::ALIGN_CENTER, pane.x + pane.w / 2,
                                   pane.y + (pane.h - rf::gr::get_font_height(fonts.body)) / 2, "Nothing to show",
                                   fonts.body);
        }
        return;
    }

    const int pad = s(12.0f);
    int top = pane.y + pad;
    if (!entry->mesh.empty()) {
        const int preview_w = pane.w - 2 * pad;
        const int preview_h = std::min(preview_w * 9 / 16, pane.h * 9 / 20);
        const UiRect preview{pane.x + pad, top, preview_w, preview_h};
        if (ctx.draw) {
            skin_preview_well(ctx, preview, g_preview.bitmap(), fonts.caption);
        }
        else {
            g_preview_rect = preview;
        }
        top += preview_h + pad;
    }

    const UiRect view{pane.x + s(4.0f), top, pane.w - s(8.0f), pane.y + pane.h - s(4.0f) - top};
    if (view.h <= 0) {
        return;
    }
    const int gutter = std::max(ctx.style.scrollbar_w_min, ctx.scaled(ctx.style.scrollbar_w)) + s(4.0f);
    const int text_x = pane.x + pad;
    const int text_w = std::max(s(80.0f), pane.x + pane.w - pad - gutter - text_x);
    const DetailLayout& lay = layout_detail(ctx, *entry, text_w, fonts);
    const int line_step = rf::gr::get_font_height(fonts.body);
    const UiId id = ui_id("reference_detail");
    UiScrollRegion region = ui_scroll_begin(ctx, view, lay.height, g_detail_scroll, line_step * 2, id, true);
    if (ctx.draw) {
        for (const DetailLine& line : lay.lines) {
            const int y = region.start_y() + line.y;
            const int line_h = line.rule ? 1 : rf::gr::get_font_height(line.font);
            if (y > view.y + view.h || y + line_h < view.y) {
                continue;
            }
            const UiRect d = ui_draw_rect(ctx, UiRect{text_x + line.x, y, line.w, 0});
            rf::gr::set_color(line.color);
            if (line.rule) {
                rf::gr::rect(d.x, d.y, d.w, std::max(1, s(1.0f)));
            }
            else {
                rf::gr::string(d.x, d.y, line.text.c_str(), line.font);
            }
        }
    }
    ui_scroll_end(ctx, region);
    if (ctx.draw) {
        skin_scrollbar(ctx, view, lay.height, g_detail_scroll, ctx.captured_id() == id);
    }
}

// ---------------------------------------------------------------------------
// Entry list
// ---------------------------------------------------------------------------

// True when the row was clicked, or Enter pressed on it.
bool list_row(UiContext& ctx, const UiRect& r, const ReferenceEntry& entry, bool selected, const Fonts& fonts, UiId id)
{
    const bool hovered = ui_hover(ctx, r, id);
    if (hovered) {
        ctx.note_hovered(id);
    }
    ctx.register_focusable(id, UiFocusKind::Activate);
    if (!ctx.draw) {
        return ctx.consume_activation(id) || (hovered && ctx.click);
    }

    const auto s = [&](float v) { return skin_px(ctx, v); };
    const UiRect d = ui_draw_rect(ctx, r);
    rf::gr::set_color(skin::border_dim);
    rf::gr::rect(d.x + s(8.0f), d.y + d.h - s(1.0f), d.w - s(8.0f), s(1.0f));
    if (selected) {
        const int inset = s(2.0f);
        skin_selection_bar(ctx, d.x, d.y + inset, d.w, d.h - 2 * inset);
    }
    const bool lit = hovered || selected || (ctx.focus_visible() && ctx.focus_id() == id);
    // Clear of the selection bar's LED.
    const int text_x = d.x + s(24.0f);
    const int text_w = std::max(20, d.x + d.w - s(12.0f) - text_x);
    const int body_h = rf::gr::get_font_height(fonts.body);
    const int caption_h = rf::gr::get_font_height(fonts.caption);
    const bool two_lines = !entry.subtitle.empty();
    const int block_h = two_lines ? body_h + s(2.0f) + caption_h : body_h;
    const int y = d.y + (d.h - block_h) / 2;
    const SkinText name = skin_fit_text(ctx, entry.name, fonts.body, text_w);
    rf::gr::set_color(lit ? skin::txt_hi : skin::txt);
    rf::gr::string(text_x, y + (body_h - rf::gr::get_font_height(name.font)) / 2, name.text.c_str(), name.font);
    if (two_lines) {
        const SkinText sub = skin_fit_text(ctx, entry.subtitle, fonts.caption, text_w);
        rf::gr::set_color(lit ? skin::txt : skin::txt_hint);
        rf::gr::string(text_x, y + body_h + s(2.0f), sub.text.c_str(), sub.font);
    }
    return false;
}

void build_list(UiContext& ctx, const UiRect& well, const Fonts& fonts)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const UiRect view{well.x + s(4.0f), well.y + s(4.0f), well.w - s(8.0f), well.h - s(8.0f)};
    // The bitmap fonts used below 480 px do not shrink with the scale.
    const int row_h = std::max(s(58.0f),
        rf::gr::get_font_height(fonts.body) + rf::gr::get_font_height(fonts.caption) + s(14.0f));
    const int gutter = std::max(ctx.style.scrollbar_w_min, ctx.scaled(ctx.style.scrollbar_w));
    const int row_w = view.w - gutter - s(4.0f);
    if (ctx.draw) {
        skin_slot(ctx, well.x, well.y, well.w, well.h);
    }

    const std::vector<ReferenceEntry>& entries = current_entries();
    const int count = static_cast<int>(entries.size());
    const int selected = g_selected[area_index(g_area)];
    float& scroll = g_list_scroll[area_index(g_area)];
    const int content_h = count * row_h;
    const UiId list_id = ui_id("reference_list");
    UiScrollRegion region = ui_scroll_begin(ctx, view, content_h, scroll, row_h, list_id, true);
    std::optional<UiRect> focus_rect;
    for (int i = 0; i < count; ++i) {
        const UiRect rect{view.x, region.start_y() + i * row_h, row_w, row_h};
        // Per area, so a focus left on a row of the previous area cannot pick a row of this one.
        const UiId id = ui_id(reference_area_label(g_area), i);
        if (ctx.focus_id() == id) {
            focus_rect = rect;
            // The keyboard picks what it lands on, so stepping through the list shows each entry.
            if (!ctx.draw && ctx.focus_visible() && i != selected) {
                g_pending_row = i;
            }
        }
        if (rect.y + rect.h < view.y || rect.y > view.y + view.h) {
            if (ui_offscreen_activatable(ctx, id)) {
                g_pending_row = i;
            }
            continue;
        }
        if (list_row(ctx, rect, entries[i], i == selected, fonts, id)) {
            g_pending_row = i;
        }
    }
    if (ctx.draw && count == 0) {
        const UiRect d = ui_draw_rect(ctx, UiRect{view.x, view.y, row_w, row_h});
        rf::gr::set_color(skin::txt_hint);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, d.x + d.w / 2,
                               d.y + (d.h - rf::gr::get_font_height(fonts.body)) / 2, "Nothing to show", fonts.body);
    }
    ui_scroll_end(ctx, region);

    if (ctx.draw) {
        skin_scrollbar(ctx, view, content_h, scroll, ctx.captured_id() == list_id);
        return;
    }
    ui_scroll_focus_into_view(ctx, view, focus_rect, scroll, g_list_scrolled_to);
}

// ---------------------------------------------------------------------------
// Screen
// ---------------------------------------------------------------------------

void build_area_panel(UiContext& ctx, const MenuPanel& panel, const Fonts& fonts)
{
    const auto s = [&](float v) { return ctx.scaled(v); };
    const UiRect& header = panel.header;
    const int x = panel.body.x;
    const int w = panel.body.w;
    const int body_y = panel.body.y;
    const int body_h = panel.body.h;

    if (ctx.draw) {
        menu_draw_panel_heading(ctx, panel, reference_area_label(g_area), fonts.heading);
        const std::size_t count = current_entries().size();
        const std::string tally = std::to_string(count) + (count == 1 ? " entry" : " entries");
        rf::gr::set_color(skin::ink_status);
        rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, header.x + header.w,
                               header.y + (header.h - rf::gr::get_font_height(fonts.body)) / 2, tally.c_str(),
                               fonts.body);
    }

    const int gap = s(16.0f);
    const int list_w = std::max(s(280.0f), (w - gap) * 17 / 50);
    build_list(ctx, UiRect{x, body_y, list_w, body_h}, fonts);
    build_detail(ctx, UiRect{x + list_w + gap, body_y, w - list_w - gap, body_h}, fonts);
}

void build_screen(UiContext& ctx)
{
    const MenuFrame frame = menu_frame(ctx, g_slide);
    const MenuLayout& lay = frame.lay;
    const Fonts& fonts = frame.fonts;
    const int panel_dx = frame.enter_dx + static_cast<int>(g_switch * static_cast<float>(frame.slide_w));
    const int top = frame.top;

    if (ctx.draw) {
        menu_draw_title_bar(ctx, lay.panel_x + frame.enter_dx, lay.panel_r + frame.enter_dx,
                            lay.rail_right + frame.enter_dx, frame.canvas_y, "REFERENCE", fonts.title);
    }

    std::array<MenuColumnItem, reference_area_count> items;
    for (int i = 0; i < reference_area_count; ++i) {
        items[i] = MenuColumnItem{reference_area_label(area_order[i]), area_order[i] == g_area};
    }
    const int clicked = menu_build_column(ctx, lay, lay.column_x + frame.col_dx, top, items,
                                          menu_stock_label(rf::strings::back).c_str(), fonts.column, "reference_area");
    if (clicked >= 0 && !ctx.draw) {
        g_pending_column = clicked;
    }

    const MenuPanel panel = menu_panel(ctx, lay, panel_dx, top, frame.canvas_y);
    if (ctx.draw) {
        menu_draw_panel(ctx, panel);
        const int button_h = menu_button_h(ctx);
        const auto slot = std::ranges::find(area_order, g_area) - area_order.begin();
        menu_draw_connector(ctx, panel, top + static_cast<int>(slot) * button_h + button_h / 2);
    }
    // A panel on its way out is only painted: anything done on it would land on the area about to replace it.
    if (ctx.draw || !g_switch_to) {
        build_area_panel(ctx, panel, fonts);
    }

    // Stock draws its side bars over its menus.
    if (ctx.draw) {
        rf::gr::reset_clip();
        rf::ui::menu_draw_side_bars();
    }
}

void commit_pending()
{
    if (const std::optional<int> column = std::exchange(g_pending_column, std::nullopt)) {
        ui_play_click();
        if (*column >= reference_area_count) {
            request_close();
            return;
        }
        show_area(area_order[*column]);
    }
    const int row = std::exchange(g_pending_row, -1);
    if (row >= 0 && !g_switch_to && row != g_selected[area_index(g_area)]) {
        select_row(row);
        ui_play_click();
    }
}

void __cdecl key_callback(int key)
{
    const int code = key & rf::KEY_MASK;
    if (code == rf::KEY_ESC) {
        ui_play_click();
        request_close();
        return;
    }
    // Hotkeys need no modifier held, as stock's do.
    if (key == code) {
        if (code == rf::KEY_B) {
            ui_play_click();
            request_close();
            return;
        }
        for (int i = 0; i < reference_area_count; ++i) {
            if (code == area_hotkeys[i]) {
                ui_play_click();
                show_area(area_order[i]);
                return;
            }
        }
    }
    g_ui.handle_nav_key(static_cast<rf::Key>(key));
}

void handle_input()
{
    menu_run_hit_test(g_ui, key_callback, build_screen);
    commit_pending();
}

void advance_animation()
{
    const float step = menu_slide_step();
    g_slide = g_closing ? std::max(0.0f, g_slide - step) : std::min(1.0f, g_slide + step);
    if (g_switch_to) {
        g_switch = std::min(1.0f, g_switch + step);
        if (g_switch >= 1.0f) {
            g_area = *g_switch_to;
            g_switch_to.reset();
            g_detail_scroll = 0.0f;
        }
    }
    else {
        g_switch = std::max(0.0f, g_switch - step);
    }
}

void render_preview()
{
    const ReferenceEntry* entry = current_entry();
    if (entry != g_preview_entry) {
        g_preview_entry = entry;
        MeshPreview::PartsFn parts;
        if (entry && entry->vehicle_type >= 0) {
            parts = [type = entry->vehicle_type](rf::VMesh* hull) {
                std::vector<MeshPreviewPart> out;
                for (VehicleRestAttachment& a : vehicle_rest_attachments(type, hull)) {
                    out.push_back({std::move(a.mesh_filename), a.pos, a.orient});
                }
                return out;
            };
        }
        g_preview.set_mesh(entry ? entry->mesh : std::string{}, parts, entry ? entry->preview_zoom : 1.0f);
    }
    g_preview.render_in_well(g_preview_rect.w, g_preview_rect.h);
}

} // namespace

void reference_menu_open()
{
    g_active = true;
    g_closing = false;
    g_slide = 0.0f;
    g_switch = 0.0f;
    g_switch_to.reset();
    g_detail_scroll = 0.0f;
    g_list_scrolled_to = 0;
    g_pending_column.reset();
    g_pending_row = -1;
    menu_ui_reset(g_ui);
    rf::snd_play(stock_sound_id::menu_slide_in, 0, 0.0f, 1.0f);
}

bool reference_menu_is_active()
{
    return g_active;
}

void reference_menu_do_frame(bool paused)
{
    advance_animation();

    // paused = 1 means a stock popup owns input this frame; it renders after us and on top.
    if (!paused && !g_closing) {
        handle_input();
    }

    // Before any of the frame's 2D: the preview switches the render target.
    render_preview();

    menu_run_draw(g_ui, build_screen);

    if (g_closing && g_slide <= 0.0f) {
        reference_menu_release();
    }
}

bool reference_menu_host_frame(bool& pending, float& slide, bool closing, bool paused)
{
    // Until it has slid away again the screen owns the frame; the host's column then slides back in.
    if (g_active) {
        reference_menu_do_frame(paused);
        return true;
    }
    const float step = menu_slide_step();
    slide = closing || pending ? std::max(0.0f, slide - step) : std::min(1.0f, slide + step);
    if (pending && slide <= 0.0f) {
        pending = false;
        reference_menu_open();
        reference_menu_do_frame(paused);
        return true;
    }
    return false;
}

void reference_menu_release()
{
    g_active = false;
    g_closing = false;
    g_preview.release();
    g_preview_entry = nullptr;
    g_preview_rect = UiRect{};
    g_detail_layout = DetailLayout{};
}
