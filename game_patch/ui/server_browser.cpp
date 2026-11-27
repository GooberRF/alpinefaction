#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>
#include <common/utils/int-utils.h>
#include "server_browser.h"
#include "menu_popup.h"
#include "menu_shell.h"
#include "settings_skin.h"
#include "ui_kit.h"
#include "../misc/alpine_settings.h"
#include "../multi/gametype.h"
#include "../multi/map_thumbnails.h"
#include "../multi/multi.h"
#include "../multi/network.h"
#include "../multi/server_browser_data.h"
#include "../multi/server_switch.h"
#include "../multi/alpine_packets.h"
#include "../rf/gameseq.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/input.h"
#include "../rf/localize.h"
#include "../rf/ui.h"

namespace
{

using Clock = std::chrono::steady_clock;

constexpr int wheel_rows = 3;
// Wider than the shared card, for the roster's columns beside a long player name.
constexpr float card_ref_w = 640.0f;

ServerFilter g_filter;
ServerSortColumn g_sort = ServerSortColumn::players;
bool g_reverse = false;
std::optional<rf::NetAddr> g_selected;

constexpr std::array<const char*, 3> tab_labels{"INTERNET", "FAVORITES", "LAN"};
constexpr std::array<const char*, 3> mod_labels{"Any mod", "Unmodded", "Modded"};

struct Chip
{
    const char* label;
    bool ServerFilter::* flag;
};
constexpr std::array<Chip, 4> chips{{
    {"Hide empty", &ServerFilter::hide_empty},
    {"Hide full", &ServerFilter::hide_full},
    {"Hide locked", &ServerFilter::hide_passworded},
    {"Alpine only", &ServerFilter::alpine_only},
}};

// Options settings, also offered on the status line.
struct StatusToggle
{
    const char* label;
    bool AlpineGameSettings::* setting;
};
constexpr std::array<StatusToggle, 2> status_toggles{{
    {"Server details", &AlpineGameSettings::server_browser_details},
    {"Auto refresh", &AlpineGameSettings::server_browser_auto_refresh},
}};

enum Column
{
    col_icons,
    col_name,
    col_type,
    col_map,
    col_players,
    col_ping,
    col_count,
};
constexpr std::array<const char*, col_count> column_labels{"", "SERVER", "TYPE", "MAP", "PLAYERS", "PING"};
constexpr std::array<std::optional<ServerSortColumn>, col_count> column_sorts{
    std::nullopt,
    ServerSortColumn::name,
    ServerSortColumn::game_type,
    ServerSortColumn::map,
    ServerSortColumn::players,
    ServerSortColumn::ping,
};

struct AfFlagBadge
{
    uint32_t flag;
    const char* text;
};
constexpr std::array<AfFlagBadge, 9> af_flag_badges{{
    {AF_GI_FLAG_ALPINE_ONLY, "Alpine only"},
    {AF_GI_FLAG_REJECT_LEGACY_CLIENTS, "No legacy clients"},
    {AF_GI_FLAG_MATCH_MODE, "Match mode"},
    {AF_GI_FLAG_STATS_ENABLED, "Stats"},
    {AF_GI_FLAG_SAVING_ENABLED, "Saving"},
    {AF_GI_FLAG_CLICK_LIMITER, "Click limiter"},
    {AF_GI_FLAG_NO_PLAYER_COLLIDE, "No player collide"},
    {AF_GI_FLAG_GAUSSIAN_SPREAD, "Gaussian spread"},
    {AF_GI_FLAG_DAMAGE_NOTIFICATIONS, "Damage numbers"},
}};

constexpr rf::Color row_hover{255, 255, 255, 14};

rf::Color ping_color(int ping)
{
    if (ping <= 80) {
        return skin::good;
    }
    if (ping <= 150) {
        return {0xD8, 0xD0, 0x5A, 255};
    }
    if (ping <= 250) {
        return {0xE8, 0x9A, 0x40, 255};
    }
    return {0xE8, 0x5A, 0x4A, 255};
}

std::string game_type_name(int game_type)
{
    return std::string{multi_game_type_name_short(static_cast<rf::NetGameType>(game_type))};
}

// A server that has not answered yet goes by its address, with nothing else to show.
std::string name_label(const ServerBrowserEntry& e)
{
    return e.has_info ? e.name : std::format("{}", e.addr);
}

std::string type_label(const ServerBrowserEntry& e)
{
    if (!e.has_info) {
        return {};
    }
    const std::string type = game_type_name(e.game_type);
    return e.mod.empty() ? type : std::format("{} : {}", type, e.mod);
}

std::string players_label(const ServerBrowserEntry& e)
{
    return e.has_info ? server_players_label(e.af ? &*e.af : nullptr, e.players, e.max_players) : std::string{};
}

std::string ping_label(int ping)
{
    return ping < 0 ? "N/R" : ping < 1000 ? std::to_string(ping) : ">999";
}

bool address_char_allowed(char c)
{
    return (c >= '0' && c <= '9') || c == '.' || c == ':';
}

// ─── Details card ───

struct CardPlayer
{
    std::string name;
    int score = 0;
    int kills = 0;
    int deaths = 0;
    int caps = 0;
    int team = -1; // 0 red, 1 blue, -1 no teams
    bool bot = false;
    bool spectator = false;
    bool idle = false;
};

struct Card
{
    std::string name;
    std::string address;
    std::string level;
    std::string level_filename;
    std::string game_type;
    std::string version;
    std::string counts;
    std::string time_left;
    std::vector<std::pair<const char*, rf::Color>> badges;
    bool team_scores = false;
    int red_score = 0;
    int blue_score = 0;
    bool show_caps = false;
    std::vector<CardPlayer> players;
    std::string players_status;
};

Card make_card(const ServerBrowserEntry& e)
{
    Card c;
    c.name = name_label(e);
    if (!e.has_info) {
        c.players_status = "This server has not answered yet";
        return c;
    }
    c.address = std::format("{}", e.addr);
    c.level = e.level;
    c.game_type = game_type_name(e.game_type);
    const auto type = static_cast<rf::NetGameType>(e.game_type);
    const auto plural = [](int n, const char* one, const char* many) {
        return std::format("{} {}", n, n == 1 ? one : many);
    };
    c.counts = server_entry_bots(e) > 0
        ? std::format("{}, {} ({} max)", plural(server_entry_humans(e), "player", "players"),
                      plural(server_entry_bots(e), "bot", "bots"), e.max_players)
        : std::format("{} ({} max)", plural(server_entry_humans(e), "player", "players"), e.max_players);

    uint32_t af_flags = 0;
    if (e.af) {
        c.version = "Alpine Faction " + server_af_version_label(*e.af);
        c.level_filename = e.af->level_filename;
        af_flags = e.af->af_flags;
    }
    const std::optional<ServerRoster>& roster = e.roster;
    if (roster) {
        if (c.level_filename.empty()) {
            c.level_filename = roster->level_filename;
        }
        if (!e.af) {
            af_flags = roster->af_flags;
        }
    }
    if (!e.dedicated) {
        // In the list's color for modded servers, as it is not a server rule.
        c.badges.emplace_back("Listen server", skin::gold);
    }
    if (server_entry_is_modded(e)) {
        c.badges.emplace_back("Modded", skin::badge_mp);
    }
    for (const AfFlagBadge& badge : af_flag_badges) {
        if (af_flags & badge.flag) {
            c.badges.emplace_back(badge.text, skin::badge_mp);
        }
    }
    switch (server_browser_roster_state(e.addr)) {
    case ServerRosterState::none:
    case ServerRosterState::asking:
        c.players_status = "Asking for players...";
        return c;
    case ServerRosterState::no_answer:
        c.players_status = "This server does not list its players";
        return c;
    case ServerRosterState::empty:
    case ServerRosterState::ok:
        break;
    }
    // af_send_player_info_response fills the team scores for these.
    c.team_scores = type == rf::NG_TYPE_CTF || type == rf::NG_TYPE_TEAMDM || type == rf::NG_TYPE_KOTH
        || type == rf::NG_TYPE_DC;
    c.red_score = roster->red_score;
    c.blue_score = roster->blue_score;
    c.show_caps = type == rf::NG_TYPE_CTF || type == rf::NG_TYPE_SAL || type == rf::NG_TYPE_SCAV;
    c.time_left = roster->time_left_seconds == UINT32_MAX
        ? std::string{"No time limit"}
        : std::format("{}:{:02} left", roster->time_left_seconds / 60, roster->time_left_seconds % 60);
    const bool teams = type < rf::NG_TYPE_UNK && multi_game_type_is_team_type(type);
    for (const ServerRosterPlayer& p : roster->players) {
        if (p.flags & AF_PLAYER_FLAG_BROWSER) {
            continue;
        }
        c.players.push_back(CardPlayer{
            .name = p.name,
            .score = p.score,
            .kills = p.kills,
            .deaths = p.deaths,
            .caps = p.caps,
            .team = teams ? ((p.flags & AF_PLAYER_FLAG_TEAM_BLUE) ? 1 : 0) : -1,
            .bot = (p.flags & AF_PLAYER_FLAG_BOT) != 0,
            .spectator = (p.flags & AF_PLAYER_FLAG_SPECTATOR) != 0,
            .idle = (p.flags & AF_PLAYER_FLAG_IDLE) != 0,
        });
    }
    std::ranges::stable_sort(c.players, [](const CardPlayer& a, const CardPlayer& b) { return a.score > b.score; });
    if (c.players.empty()) {
        c.players_status = "Nobody is playing";
    }
    return c;
}

void draw_card(const UiContext& ctx, const Card& card, const UiRect& row, int anchor_x, const UiRect& bounds)
{
    const auto s = [&](float v) { return ctx.scaled(v); };
    const int font = menu_font(false, 18.0f, ctx.scale());
    const int bold = menu_font(true, 20.0f, ctx.scale());
    const int small_font = menu_font(false, 15.0f, ctx.scale());
    const int fh = rf::gr::get_font_height(font);
    const int bold_h = rf::gr::get_font_height(bold);
    const int small_h = rf::gr::get_font_height(small_font);
    // This face's line height already leaves room between lines.
    const int line_h = fh;
    const MenuCard frame = menu_card(ctx, bounds, card_ref_w);
    const int pad = frame.pad;
    const int inner = frame.inner;
    // The preview goes beside the name and info, leaving the height to the players. A level known to have no preview
    // keeps only the caption band, across the card, and a server that has not answered has neither.
    const bool answered = !card.game_type.empty();
    const bool has_thumb = answered && !map_thumbnail_missing(card.level_filename);
    const int cap_font = has_thumb ? small_font : font;
    const int cap_h = rf::gr::get_font_height(cap_font) + s(8.0f);
    const int band_block = answered && !has_thumb ? cap_h + s(8.0f) : 0;
    const int thumb_w = has_thumb ? inner * 3 / 8 : 0;
    const int text_dx = has_thumb ? thumb_w + s(12.0f) : 0;
    const int text_w = inner - text_dx;

    std::vector<std::string> name_lines = ui_wrap_text(card.name, text_w, bold);
    if (name_lines.empty()) {
        name_lines.emplace_back();
    }
    // Two lines at most, so a long name cannot starve the players. The wrap broke before the third line's first word,
    // so joining it back makes the second line too wide, and ui_fit_end marks the cut.
    if (name_lines.size() > 2) {
        name_lines[1] += ' ' + name_lines[2];
        name_lines.resize(2);
    }
    for (std::string& line : name_lines) {
        line = ui_fit_end(line, text_w, bold);
    }
    const std::array<std::pair<const std::string*, rf::Color>, 5> info_lines{{
        {&card.level, skin::txt},
        {&card.address, skin::txt_hint},
        {&card.version, skin::txt_hint},
        {&card.counts, skin::txt},
        {&card.time_left, skin::txt},
    }};
    const auto info_count = std::ranges::count_if(info_lines, [](const auto& line) { return !line.first->empty(); });
    const int text_h = static_cast<int>(name_lines.size()) * bold_h + s(4.0f) + static_cast<int>(info_count) * small_h;
    const int top_h = std::max(text_h, thumb_w * 9 / 16);
    const int badge_h = s(skin::badge_h);
    int badge_rows = 0;
    {
        int x = 0;
        for (const auto& badge : card.badges) {
            const int bw = skin_badge_width(ctx, badge.first, small_font, false);
            if (badge_rows == 0 || x + bw > inner) {
                ++badge_rows;
                x = 0;
            }
            x += bw + s(6.0f);
        }
    }
    const int fixed_h = 2 * pad + band_block + top_h + s(6.0f) + badge_rows * (badge_h + s(4.0f)) + s(8.0f);

    // Each team (or everyone), then the humans spectating. Bots are only counted.
    struct Section
    {
        int team = -1; // >= 0: under that team's header
        std::vector<int> humans;
        int bots = 0;
        int shown = 0;
    };
    const bool teams = std::ranges::any_of(card.players, [](const CardPlayer& p) { return p.team >= 0; });
    std::vector<Section> sections(teams ? 3 : 2);
    if (teams) {
        sections[0].team = 0;
        sections[1].team = 1;
    }
    for (int i = 0; i < static_cast<int>(card.players.size()); ++i) {
        const CardPlayer& p = card.players[i];
        Section& own = sections[teams && p.team == 1 ? 1 : 0];
        if (p.bot) {
            ++own.bots;
        }
        else {
            (p.spectator ? sections.back() : own).humans.push_back(i);
        }
    }
    int humans = 0;
    int fixed_lines = 0;
    for (const Section& sec : sections) {
        humans += static_cast<int>(sec.humans.size());
        fixed_lines += (sec.team >= 0 ? 1 : 0) + (sec.bots > 0 ? 1 : 0);
    }
    const bool has_table = !card.players.empty();
    const int header_h = line_h + s(8.0f);
    const int fit = std::max(0, (bounds.h - 2 * pad - fixed_h - header_h) / line_h);
    // Short of room, humans give way rather than the team headers and bot counts, spectators first.
    const bool more = humans > 0 && fixed_lines + humans > fit;
    int budget = more ? std::max(0, fit - fixed_lines - 1) : humans;
    const auto share = [&](std::span<Section> group) {
        // In turns, so one team does not take the other's rows.
        for (bool took = true; took && budget > 0;) {
            took = false;
            for (Section& sec : group) {
                if (budget > 0 && sec.shown < static_cast<int>(sec.humans.size())) {
                    ++sec.shown;
                    --budget;
                    took = true;
                }
            }
        }
    };
    share(std::span{sections}.first(sections.size() - 1));
    share(std::span{sections}.last(1));
    int shown = 0;
    for (const Section& sec : sections) {
        shown += sec.shown;
    }
    const int h = fixed_h + (has_table ? header_h + (fixed_lines + shown + (more ? 1 : 0)) * line_h : line_h);

    const UiRect r = menu_card_place(ctx, frame, h, row, anchor_x, bounds);
    menu_draw_card(ctx, r);

    const int cx = r.x + pad;
    int cy = r.y + pad;
    if (answered) {
        const std::string level = level_filename_without_ext(card.level_filename);
        const MenuLevelCaption caption{
            .level = level,
            .type = card.game_type,
            .font = cap_font,
            .cap_h = cap_h,
            .pad = s(8.0f),
            .level_color = skin::txt_hi,
            .type_color = skin::txt,
        };
        const UiRect preview = has_thumb ? UiRect{cx, cy, thumb_w, top_h} : UiRect{cx, cy, inner, cap_h};
        menu_draw_level_preview(card.level_filename, preview, caption);
    }
    cy += band_block;
    {
        const int tx = cx + text_dx;
        int ty = cy;
        for (const std::string& line : name_lines) {
            skin_draw_text(tx, ty, line, bold, skin::txt_hi);
            ty += bold_h;
        }
        ty += s(4.0f);
        for (const auto& [text, color] : info_lines) {
            if (!text->empty()) {
                skin_draw_text(tx, ty, ui_fit_end(*text, text_w, small_font), small_font, color);
                ty += small_h;
            }
        }
    }
    cy += top_h + s(6.0f);
    {
        int bx = cx;
        bool first = true;
        for (const auto& [badge, color] : card.badges) {
            const int bw = skin_badge_width(ctx, badge, small_font, false);
            if (first || bx + bw > cx + inner) {
                if (!first) {
                    cy += badge_h + s(4.0f);
                }
                bx = cx;
                first = false;
            }
            skin_badge(ctx, UiRect{bx, cy, bw, badge_h}, badge, color, small_font, nullptr, false);
            bx += bw + s(6.0f);
        }
        if (!first) {
            cy += badge_h + s(4.0f);
        }
    }
    cy += s(4.0f);
    rf::gr::set_color(skin::border_dim);
    rf::gr::rect(cx, cy, inner, 1);
    cy += s(4.0f);

    if (!has_table) {
        skin_draw_text(cx, cy, ui_fit_end(card.players_status, inner, font), font, skin::txt_hint);
        return;
    }
    const int col_gap = s(12.0f);
    const int kd_r = cx + inner;
    const int kd_w = rf::gr::get_string_size("000/000", font).first;
    const int caps_r = kd_r - kd_w - col_gap;
    const int score_x = (card.show_caps ? caps_r - rf::gr::get_string_size("Caps", font).first : kd_r - kd_w) - col_gap;
    const int name_w = score_x - rf::gr::get_string_size("Score", font).first - col_gap - cx;
    rf::gr::set_color(skin::txt_hint);
    rf::gr::string(cx, cy, "Player", font);
    rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, score_x, cy, "Score", font);
    if (card.show_caps) {
        rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, caps_r, cy, "Caps", font);
    }
    rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, kd_r, cy, "K/D", font);
    rf::gr::set_color(skin::border_dim);
    rf::gr::rect(cx, cy + line_h + s(2.0f), inner, 1);
    cy += header_h;
    for (const Section& sec : sections) {
        const int indent = sec.team >= 0 ? s(10.0f) : 0;
        if (sec.team >= 0) {
            const rf::Color color = sec.team == 0 ? skin::team_red : skin::team_blue;
            skin_draw_text(cx, cy, sec.team == 0 ? "RED TEAM" : "BLUE TEAM", font, color);
            if (card.team_scores) {
                const int score = sec.team == 0 ? card.red_score : card.blue_score;
                rf::gr::set_color(color);
                rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, score_x, cy, std::to_string(score).c_str(), font);
            }
            cy += line_h;
        }
        for (int i = 0; i < sec.shown; ++i) {
            const CardPlayer& p = card.players[sec.humans[i]];
            const std::string name = p.spectator ? p.name + " (spectating)" : p.idle ? p.name + " (idle)" : p.name;
            skin_draw_text(cx + indent, cy, ui_fit_end(name, std::max(20, name_w - indent), font), font,
                           p.spectator || p.idle ? skin::txt_hint : skin::txt_hi);
            rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, score_x, cy, std::to_string(p.score).c_str(), font);
            rf::gr::set_color(skin::txt);
            if (card.show_caps) {
                rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, caps_r, cy, std::to_string(p.caps).c_str(), font);
            }
            rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, kd_r, cy, std::format("{}/{}", p.kills, p.deaths).c_str(),
                                   font);
            cy += line_h;
        }
        if (sec.bots > 0) {
            skin_draw_text(cx + indent, cy, std::format("+ {} {}", sec.bots, sec.bots == 1 ? "bot" : "bots"), font,
                           skin::txt_hint);
            cy += line_h;
        }
    }
    if (more) {
        skin_draw_text(cx, cy, std::format("+{} more", humans - shown), font, skin::txt_hint);
    }
}

} // namespace

// ─── Lifetime ───

void ServerBrowserPanel::open()
{
    open_ = true;
    keyboard_selection_ = false;
    last_click_entry_ = -1;
    hover_.clear();
    actions_.clear();
    favorite_toggle_ = -1;
    card_shown_ = -1;
    server_browser_open(g_filter.tab, g_alpine_game_config.server_browser_auto_refresh);
    refresh_view();
    scroll_to_selection_ = true;
}

void ServerBrowserPanel::close()
{
    if (!open_) {
        return;
    }
    open_ = false;
    hover_.clear();
    server_browser_close();
    map_thumbnails_release_bitmaps();
}

bool ServerBrowserPanel::back()
{
    close();
    return true;
}

void ServerBrowserPanel::frame()
{
    if (!open_) {
        return;
    }
    server_browser_frame();
    // A row never moves under the cursor; the keyboard's card rides its row, held where it is on screen.
    if (hover_.key() < 0) {
        const int offset = keyboard_selection_ ? selected_row() - top_ : -1;
        refresh_view();
        if (const int row = selected_row(); offset >= 0 && offset < visible_ && row >= 0) {
            top_ = std::clamp(row - offset, 0, std::max(0, static_cast<int>(view_.size()) - visible_));
        }
    }
    // Not while the menus only pass through on a join or a level start, nor under a stock popup.
    if (!g_alpine_game_config.server_browser_help_seen && !popup_.active() && !rf::ui::popup_is_active()
        && !rf::multi_join_game_is_connecting() && rf::gameseq_get_pending_state() == rf::GS_NONE) {
        g_alpine_game_config.server_browser_help_seen = true;
        notice(rf::strings::server_list, rf::strings::server_list_help);
    }
    const int entry = card_entry();
    if (entry >= 0) {
        const rf::NetAddr addr = server_browser_entries()[entry].addr;
        if (g_alpine_game_config.server_browser_auto_refresh) {
            server_browser_refresh_quiet(addr);
            server_browser_request_roster(addr);
        }
        else if (entry != card_shown_) {
            // Once as the card comes up, which would otherwise show no players.
            server_browser_request_roster(addr);
        }
    }
    card_shown_ = entry;
}

void ServerBrowserPanel::refresh_view()
{
    view_ = server_browser_view(server_browser_entries(), g_filter, g_sort, g_reverse);
    top_ = std::clamp(top_, 0, std::max(0, static_cast<int>(view_.size()) - visible_));
}

int ServerBrowserPanel::selected_entry() const
{
    const int row = selected_row();
    return row >= 0 ? view_[row] : -1;
}

int ServerBrowserPanel::selected_row() const
{
    if (!g_selected) {
        return -1;
    }
    const int entry = server_browser_find(*g_selected);
    const auto it = std::ranges::find(view_, entry);
    return entry >= 0 && it != view_.end() ? static_cast<int>(it - view_.begin()) : -1;
}

int ServerBrowserPanel::card_entry() const
{
    if (!open_ || !g_alpine_game_config.server_browser_details || popup_.active() || rf::ui::popup_is_active()) {
        return -1;
    }
    if (hover_.rested()) {
        return hover_.key();
    }
    // The keyboard's card needs its row in view, and stands aside while the cursor is on the panel's controls, which
    // it could cover.
    const int row = selected_row();
    if (!keyboard_selection_ || pointer_on_controls_ || row < top_ || row >= top_ + visible_) {
        return -1;
    }
    const int selected = view_[row];
    return hover_.key() < 0 || hover_.key() == selected ? selected : -1;
}

void ServerBrowserPanel::move_selection(int to_row)
{
    if (view_.empty()) {
        return;
    }
    const int row = std::clamp(to_row, 0, static_cast<int>(view_.size()) - 1);
    g_selected = server_browser_entries()[view_[row]].addr;
    keyboard_selection_ = true;
    scroll_to_selection_ = true;
}

// ─── Input ───

bool ServerBrowserPanel::key(UiContext& ctx, int key)
{
    const int code = key & rf::KEY_MASK;
    if (key != code) {
        return false;
    }
    const int row = selected_row();
    switch (code) {
    case rf::KEY_UP:
        move_selection(row < 0 ? 0 : row - 1);
        return true;
    case rf::KEY_DOWN:
        move_selection(row < 0 ? 0 : row + 1);
        return true;
    case rf::KEY_PAGEUP:
        move_selection(row < 0 ? 0 : row - visible_);
        return true;
    case rf::KEY_PAGEDOWN:
        move_selection(row < 0 ? 0 : row + visible_);
        return true;
    case rf::KEY_HOME:
        move_selection(0);
        return true;
    case rf::KEY_END:
        move_selection(static_cast<int>(view_.size()) - 1);
        return true;
    case rf::KEY_ENTER:
    case rf::KEY_PADENTER:
        // A widget the keyboard is on takes Enter itself.
        if (ctx.focus_visible() && ctx.focus_id() != 0) {
            return false;
        }
        actions_.push_back(Action::join);
        return true;
    case rf::KEY_J:
        actions_.push_back(Action::join);
        return true;
    case rf::KEY_G:
        actions_.push_back(Action::get_servers);
        return true;
    case rf::KEY_R:
        actions_.push_back(Action::refresh_all);
        return true;
    case rf::KEY_A:
        actions_.push_back(Action::add_server);
        return true;
    case rf::KEY_F:
        actions_.push_back(Action::favorite);
        return true;
    default:
        return false;
    }
}

void ServerBrowserPanel::hints(const UiContext& ctx, std::vector<MenuHint>& hints) const
{
    if (ctx.focused_text_id() != 0) {
        return;
    }
    // As key() and run(): the arrows always move the selection, Enter joins unless a widget the keyboard is on takes
    // it, and the rest need a selection or an idle browser.
    const bool selected = selected_entry() >= 0;
    if (view_.empty()) {
        std::erase_if(hints, [](const MenuHint& hint) { return hint.arrows == MenuHintArrows::UpDown; });
    }
    if (!ctx.focus_visible() || ctx.focus_id() == 0) {
        std::erase_if(hints, [](const MenuHint& hint) { return hint.key == "ENTER"; });
        if (selected) {
            hints.push_back({"ENTER", "Join"});
        }
    }
    if (selected) {
        hints.push_back({"F", "Favorite"});
    }
    if (!server_browser_busy()) {
        hints.push_back({"R", "Refresh all"});
        hints.push_back({"G", "Get servers"});
    }
    hints.push_back({"A", "Add server"});
    if (!view_.empty()) {
        hints.push_back({"PGUP/PGDN", "Page"});
        hints.push_back({"HOME/END", "First / last"});
    }
}

void ServerBrowserPanel::commit()
{
    const bool acted = !actions_.empty() || favorite_toggle_ >= 0;
    for (const Action action : std::exchange(actions_, {})) {
        // A join can close the panel.
        if (!open_ || popup_.active()) {
            break;
        }
        run(action);
    }
    if (const int entry = std::exchange(favorite_toggle_, -1); entry >= 0) {
        toggle_favorite(entry);
    }
    if (std::exchange(rebuild_view_, false) || acted) {
        refresh_view();
    }
}

void ServerBrowserPanel::run(Action action)
{
    const bool busy = server_browser_busy();
    switch (action) {
    case Action::join:
        if (const int entry = selected_entry(); entry >= 0) {
            ui_play_click();
            request_join(entry);
        }
        break;
    case Action::get_servers:
        if (!busy) {
            ui_play_click();
            server_browser_get_servers();
        }
        break;
    case Action::refresh_all:
        if (!busy) {
            ui_play_click();
            server_browser_refresh(g_filter.tab);
        }
        break;
    case Action::add_server:
        ui_play_click();
        popup_.open_input(rf::strings::add_server, rf::strings::enter_server_address, "", 21, address_char_allowed,
                          "ADD", [this](int button, const std::string& text) {
                              if (button != 1) {
                                  return;
                              }
                              switch (server_browser_add_server(text)) {
                              case AddServerResult::added:
                                  // Neither the LAN tab nor, until it answers, the Internet tab lists it.
                                  if (g_filter.tab != ServerTab::favorites) {
                                      g_filter.tab = ServerTab::favorites;
                                      top_ = 0;
                                      server_browser_refresh(g_filter.tab);
                                  }
                                  g_selected = parse_server_address(text);
                                  keyboard_selection_ = true;
                                  scroll_to_selection_ = true;
                                  rebuild_view_ = true;
                                  break;
                              case AddServerResult::invalid:
                                  notice(rf::strings::error, "Give the address as a.b.c.d or a.b.c.d:port.");
                                  break;
                              case AddServerResult::favorites_full:
                                  notice(rf::strings::error, "The favorites list is full.");
                                  break;
                              }
                          });
        break;
    case Action::favorite:
        if (const int entry = selected_entry(); entry >= 0) {
            toggle_favorite(entry);
        }
        break;
    }
}

void ServerBrowserPanel::toggle_favorite(int entry)
{
    const ServerBrowserEntry& e = server_browser_entries()[entry];
    const bool favorite = !e.favorite;
    if (!server_browser_set_favorite(e.addr, favorite)) {
        notice(rf::strings::error, "The favorites list is full.");
        return;
    }
    ui_play_toggle(favorite);
}

void ServerBrowserPanel::notice(const char* title, std::string text)
{
    popup_.open(title, std::move(text), {{"OK"}}, 0, 0, nullptr, MenuPopupStyle::Neutral);
}

void ServerBrowserPanel::request_join(int entry)
{
    const ServerBrowserEntry& e = server_browser_entries()[entry];
    if (server_browser_is_current_server(e.addr)) {
        close();
        return;
    }
    if (e.incompatible) {
        notice(rf::strings::error, rf::strings::incompatible_version);
        return;
    }
    if (const std::optional<std::string> refusal = server_join_mod_mismatch(e.mod)) {
        notice(rf::strings::error, *refusal);
        return;
    }
    if (e.unknown_game_type) {
        show_unsupported_game_type_popup();
        return;
    }
    const rf::NetAddr addr = e.addr;
    const std::string name = server_browser_display_name(addr);
    if (e.passworded) {
        popup_.open_input(rf::strings::enter_password, "", "", 32, ui_char_filter_printable, "JOIN",
                          [this, addr, name](int button, const std::string& text) {
                              if (button == 1) {
                                  confirm_join(addr, name, text);
                              }
                          });
        return;
    }
    confirm_join(addr, name, "");
}

void ServerBrowserPanel::confirm_join(const rf::NetAddr& addr, const std::string& name, const std::string& password)
{
    if (!server_join_needs_confirmation()) {
        server_join(addr, password);
        return;
    }
    const ServerJoinConfirmation confirmation = server_join_confirmation(name);
    // CANCEL on Enter, as the console's connect asks.
    popup_.open(confirmation.title, confirmation.text, {{"CANCEL"}, {"JOIN", rf::KEY_J}}, 0, 0,
                [addr, password](int button, const std::string&) {
                    if (button == 1) {
                        server_join(addr, password);
                    }
                });
}

// ─── Building ───

void ServerBrowserPanel::build(UiContext& ctx, const MenuPanel& panel, const MenuFonts& fonts)
{
    const float scale = ctx.scale();
    const auto s = [&](float v) { return ctx.scaled(v); };
    const int body_font = fonts.body;
    const int tab_font = menu_font(true, 20.0f, scale);
    const int chip_font = menu_font(false, 18.0f, scale);
    const int row_font = menu_font(false, 17.0f, scale);
    const int head_font = menu_font(true, 16.0f, scale);
    const int button_font = menu_font(true, 20.0f, scale);
    const int content_x = panel.content_x;
    const int content_w = panel.content_r - panel.content_x;
    const std::vector<ServerBrowserEntry>& entries = server_browser_entries();
    // The panel on its way out is only painted.
    const bool live = open_;

    // The type and mod cyclers at the right of the filter line, the search box spanning them above.
    int widest_type = rf::gr::get_string_size("All types", chip_font).first;
    for (int type = 0; type < rf::NG_TYPE_UNK; ++type) {
        widest_type = std::max(widest_type, rf::gr::get_string_size(game_type_name(type), chip_font).first);
    }
    int widest_mod = 0;
    for (const char* label : mod_labels) {
        widest_mod = std::max(widest_mod, rf::gr::get_string_size(label, chip_font).first);
    }
    const int type_w = skin_cycler_width(ctx, widest_type);
    const int mod_w = skin_cycler_width(ctx, widest_mod);
    const int cyclers_x = content_x + content_w - type_w - s(12.0f) - mod_w;

    {
        const UiRect& line = panel.header;
        int x = line.x;
        for (int t = 0; t < static_cast<int>(tab_labels.size()); ++t) {
            const auto tab = static_cast<ServerTab>(t);
            const UiId id = ui_id("server_tab", t);
            const UiRect r{x, line.y, rf::gr::get_string_size(tab_labels[t], tab_font).first + s(28.0f), line.h};
            const bool active = g_filter.tab == tab;
            const bool hovered = ui_hover(ctx, r, id);
            if (hovered) {
                ctx.note_hovered(id);
            }
            ctx.register_focusable(id, UiFocusKind::Activate);
            if (ctx.draw) {
                const int ty = r.y + (r.h - rf::gr::get_font_height(tab_font)) / 2;
                skin_draw_text(r.x, ty, tab_labels[t], tab_font,
                               active || hovered ? skin::ink_title : skin::ink_status);
                if (active) {
                    rf::gr::set_color(skin::ink_title);
                    rf::gr::rect(r.x, ty + rf::gr::get_font_height(tab_font) + s(2.0f), r.w - s(28.0f),
                                 std::max(1, s(3.0f)));
                }
                if (ctx.focus_visible() && ctx.focus_id() == id) {
                    skin_focus_ring(ctx, r.x, r.y, r.w - s(20.0f), r.h);
                }
            }
            else if (live && !active && (ctx.consume_activation(id) || (hovered && ctx.click))) {
                g_filter.tab = tab;
                top_ = 0;
                rebuild_view_ = true;
                server_browser_refresh(tab);
                ui_play_click();
            }
            x = r.x + r.w;
        }
        const int search_x = std::max(cyclers_x, x + s(20.0f));
        const int field_h = s(skin::widget_h);
        const UiRect search{search_x, line.y + (line.h - field_h) / 2,
                            std::max(0, content_x + content_w - search_x), field_h};
        if (skin_search_box(ctx, search, ui_id("server_search"), g_filter.search, body_font, "Search servers...")) {
            rebuild_view_ = true;
        }
        if (ctx.draw) {
            menu_draw_panel_ridge(ctx, panel);
        }
    }

    int y = panel.body.y;
    {
        const int h = s(skin::widget_h);
        const UiRect type_r{content_x + content_w - type_w, y, type_w, h};
        const UiRect mod_r{cyclers_x, y, mod_w, h};
        // A chip that would run under the cyclers starts a line of its own.
        int x = content_x;
        int chips_y = y;
        for (int i = 0; i < static_cast<int>(chips.size()); ++i) {
            const Chip& chip = chips[i];
            const int w = skin_lamp_toggle_width(ctx, chip.label, chip_font);
            const int right = chips_y == y ? mod_r.x - s(16.0f) : content_x + content_w;
            if (x > content_x && x + w > right) {
                x = content_x;
                chips_y += h + s(10.0f);
            }
            const UiRect r{x, chips_y, w, h};
            if (skin_lamp_toggle(ctx, r, chip.label, g_filter.*chip.flag, chip_font, ui_id("server_chip", i))
                && live) {
                g_filter.*chip.flag = !(g_filter.*chip.flag);
                top_ = 0;
                rebuild_view_ = true;
            }
            x = r.x + r.w + s(16.0f);
        }
        const std::string type_value =
            g_filter.game_type < 0 ? std::string{"All types"} : game_type_name(g_filter.game_type);
        if (const int step = skin_cycler(ctx, type_r, type_value.c_str(), chip_font, true, ui_id("server_type"), false);
            step != 0 && live) {
            const int count = rf::NG_TYPE_UNK + 1;
            g_filter.game_type = wrap_step(g_filter.game_type + 1, step, count) - 1;
            top_ = 0;
            rebuild_view_ = true;
            ui_play_click();
        }
        const int mod_index = static_cast<int>(g_filter.mod);
        if (const int step =
                skin_cycler(ctx, mod_r, mod_labels[mod_index], chip_font, true, ui_id("server_mod"), false);
            step != 0 && live) {
            const int count = static_cast<int>(mod_labels.size());
            g_filter.mod = static_cast<ServerModFilter>(wrap_step(mod_index, step, count));
            top_ = 0;
            rebuild_view_ = true;
            ui_play_click();
        }
        y = chips_y + h + s(10.0f);
    }

    const int footer_y = panel.inset.y + panel.inset.h - s(menu_panel_footer_inset);
    const int footer_h = s(menu_panel_footer_h);
    const int button_h = s(skin::widget_h + 6.0f);
    const int buttons_y = footer_y - s(10.0f) - button_h;
    const bool busy = server_browser_busy();
    const int selected = selected_entry();
    {
        struct Button
        {
            const char* label;
            Action action;
            bool enabled;
        };
        const std::array<Button, 5> buttons{{
            {rf::strings::join, Action::join, selected >= 0},
            {rf::strings::get_servers, Action::get_servers, !busy},
            {rf::strings::refresh_all, Action::refresh_all, !busy},
            {rf::strings::add_server, Action::add_server, true},
            {"Favorite", Action::favorite, selected >= 0},
        }};
        const int count = static_cast<int>(buttons.size());
        const int gap = s(12.0f);
        const int w = (content_w - (count - 1) * gap) / count;
        std::string widest;
        std::vector<std::string> labels;
        for (const Button& b : buttons) {
            labels.push_back(menu_stock_label(b.label));
            if (rf::gr::get_string_size(labels.back(), button_font).first
                > rf::gr::get_string_size(widest, button_font).first) {
                widest = labels.back();
            }
        }
        const int font = skin_fit_text(ctx, widest, button_font, w - s(20.0f)).font;
        for (int i = 0; i < count; ++i) {
            const UiRect r{content_x + i * (w + gap), buttons_y, i == count - 1 ? content_w - i * (w + gap) : w,
                           button_h};
            if (skin_button(ctx, r, labels[i].c_str(), font, SkinButton::Secondary, buttons[i].enabled,
                            ui_id("server_button", i))
                && live) {
                actions_.push_back(buttons[i].action);
            }
        }
    }

    const int heading_h = s(36.0f);
    const int row_h = std::max(1, s(32.0f));
    const int inset = std::max(1, s(2.0f));
    const int list_top = y + heading_h + s(4.0f);
    const int list_bottom = buttons_y - s(12.0f);
    visible_ = std::max(1, (list_bottom - list_top - 2 * inset) / row_h);
    const int n = static_cast<int>(view_.size());
    const int sel_row = selected_row();
    if (!ctx.draw && std::exchange(scroll_to_selection_, false) && sel_row >= 0) {
        if (sel_row < top_) {
            top_ = sel_row;
        }
        else if (sel_row >= top_ + visible_) {
            top_ = sel_row - visible_ + 1;
        }
    }
    const int max_top = std::max(0, n - visible_);
    top_ = std::clamp(top_, 0, max_top);
    const UiRect list{content_x, list_top, content_w, visible_ * row_h + 2 * inset};
    const UiRect view{list.x + inset, list.y + inset, list.w - 2 * inset, visible_ * row_h};
    const int content_h = n * row_h;
    const bool has_bar = content_h > view.h;
    const UiScrollbarGeometry bar =
        ui_scrollbar_geometry(ctx, view, content_h, static_cast<float>(top_ * row_h));
    const int rows_w = has_bar ? bar.gutter.x - view.x - s(4.0f) : view.w;
    const int pad = s(10.0f);
    const int icon = s(16.0f);

    // Fixed columns, whatever the servers hold or whether the list scrolls: the name and the map share what the others
    // leave.
    std::array<int, col_count + 1> col_x{};
    {
        const int gap = s(18.0f);
        const auto heading = [&](int c) {
            return rf::gr::get_string_size(column_labels[c], head_font).first + s(18.0f);
        };
        const int players_w = std::max(heading(col_players), rf::gr::get_string_size("32+32/32", row_font).first);
        const int ping_w = std::max(heading(col_ping), rf::gr::get_string_size(">999", row_font).first);
        const int span = bar.gutter.x - view.x - s(4.0f) - 2 * pad;
        const int type_w = std::max(heading(col_type), span / 7);
        const int icons_w = 2 * icon + s(8.0f);
        const int fixed = icons_w + type_w + players_w + ping_w + (col_count - 1) * gap;
        const int shared = std::max(s(260.0f), span - fixed);
        const int name_w = shared * 58 / 100;
        const std::array<int, col_count> widths{icons_w, name_w, type_w, shared - name_w, players_w, ping_w};
        col_x[0] = view.x + pad;
        for (int c = 0; c < col_count; ++c) {
            col_x[c + 1] = col_x[c] + widths[c] + (c < col_count - 1 ? gap : 0);
        }
    }
    const auto col_w = [&](int c) { return col_x[c + 1] - col_x[c] - (c < col_count - 1 ? s(18.0f) : 0); };

    const UiRect headings{content_x, y, content_w, heading_h};
    for (int c = 0; c < col_count; ++c) {
        const UiRect h{col_x[c], y, col_x[c + 1] - col_x[c], heading_h};
        const std::optional<ServerSortColumn> sort = column_sorts[c];
        const bool hovered = sort && ui_hover(ctx, h);
        if (!ctx.draw) {
            if (live && sort && hovered && ctx.click) {
                g_reverse = g_sort == *sort ? !g_reverse : false;
                g_sort = *sort;
                rebuild_view_ = true;
                ui_play_click();
            }
            continue;
        }
        const bool sorted = sort && g_sort == *sort;
        const int arrow = sorted ? s(14.0f) : 0;
        const SkinText t = skin_fit_text(ctx, column_labels[c], head_font, std::max(0, col_w(c) - arrow));
        const int tw = rf::gr::get_string_size(t.text, t.font).first;
        const int tx = c == col_ping ? h.x + col_w(c) - tw - arrow : h.x;
        skin_draw_text(tx, y + (heading_h - rf::gr::get_font_height(t.font)) / 2, t.text, t.font,
                       sorted || hovered ? skin::ink_title : skin::ink_status);
        if (sorted) {
            skin_arrow(g_reverse ? SkinArrow::Up : SkinArrow::Down, tx + tw + s(2.0f), y + (heading_h - arrow) / 2,
                       arrow, skin::ink_title);
        }
    }
    if (ctx.draw) {
        skin_ridge(ctx, panel.inset.x + s(8.0f), y + heading_h + s(1.0f), panel.inset.w - s(16.0f));
        skin_slot(ctx, list.x, list.y, list.w, list.h);
    }

    int hover_entry = -1;
    std::optional<UiRect> hover_rect;
    std::optional<UiRect> selected_rect;
    for (int i = 0; i < visible_ && top_ + i < n; ++i) {
        const int entry = view_[top_ + i];
        const ServerBrowserEntry& e = entries[entry];
        const UiRect r{view.x, view.y + i * row_h, rows_w, row_h};
        const bool over = ui_hover(ctx, r);
        if (over) {
            hover_entry = entry;
            hover_rect = r;
        }
        if (entry == selected) {
            selected_rect = r;
        }
        if (!ctx.draw) {
            if (live && over && ctx.click) {
                const Clock::time_point now = Clock::now();
                const std::chrono::milliseconds double_click{GetDoubleClickTime()};
                if (ctx.mx >= col_x[col_icons] && ctx.mx < col_x[col_name]) {
                    favorite_toggle_ = entry;
                    last_click_entry_ = -1;
                }
                else if (entry == last_click_entry_ && now - last_click_ < double_click) {
                    g_selected = e.addr;
                    actions_.push_back(Action::join);
                    last_click_entry_ = -1;
                }
                else {
                    last_click_entry_ = entry;
                    last_click_ = now;
                }
                g_selected = e.addr;
                keyboard_selection_ = false;
            }
            continue;
        }
        const bool sel = entry == selected;
        const bool modded = server_entry_is_modded(e);
        if (sel) {
            skin_selection_bar(ctx, r.x, r.y, r.w, r.h, false);
        }
        else if (over) {
            rf::gr::set_color(row_hover);
            rf::gr::rect(r.x, r.y, r.w, r.h);
        }
        const rf::Color ink = sel ? skin::txt_hi : modded ? skin::gold : skin::txt;
        const rf::Color dim = sel ? skin::txt : modded ? skin::gold : skin::txt_hint;
        const int ty = r.y + (row_h - rf::gr::get_font_height(row_font)) / 2;
        const auto cell = [&](int c, const std::string& text, rf::Color color) {
            skin_draw_text(col_x[c], ty, ui_fit_end(text, col_w(c), row_font), row_font, color);
        };
        const int ix = col_x[col_icons];
        skin_led(ctx, ix + icon / 2, r.y + row_h / 2, 7.0f, 16.0f, e.favorite);
        if (e.passworded) {
            skin_icon("af_padlock_icon", ix + icon + s(4.0f), r.y + (row_h - icon) / 2, icon, ink);
        }
        cell(col_name, name_label(e), ink);
        cell(col_type, type_label(e), ink);
        cell(col_map, e.level, dim);
        cell(col_players, players_label(e), ink);
        rf::gr::set_color(e.ping_ms < 0 ? skin::txt_hint : ping_color(e.ping_ms));
        rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, col_x[col_ping] + col_w(col_ping), ty,
                               ping_label(e.ping_ms).c_str(), row_font);
    }
    if (ctx.draw && n == 0) {
        const char* empty = busy ? "" : g_filter.tab == ServerTab::lan ? "No servers answered on the LAN"
                                                                      : "No servers to show";
        rf::gr::set_color(skin::txt_hint);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, view.x + view.w / 2, view.y + s(12.0f), empty, body_font);
    }
    // The card's room, clear of the button row.
    const UiRect bounds{panel.inset.x, panel.inset.y, panel.inset.w, buttons_y - panel.inset.y};
    if (!ctx.draw) {
        hover_.track(hover_entry);
        pointer_on_controls_ = ui_point_in(ctx.mx, ctx.my, bounds)
            && !ui_point_in(ctx.mx, ctx.my, UiRect{view.x, view.y, rows_w, view.h});
    }
    else if (const int card_index = card_entry(); card_index >= 0) {
        const bool hovered_card = card_index == hover_entry && hover_rect;
        const std::optional<UiRect> anchor =
            hovered_card ? hover_rect : (card_index == selected ? selected_rect : std::nullopt);
        if (anchor) {
            // The keyboard's card leaves the server's name in view.
            const int anchor_x = hovered_card ? ctx.mx : col_x[col_type];
            ctx.push_overlay([&ctx, card = make_card(entries[card_index]), row = *anchor, anchor_x, bounds] {
                draw_card(ctx, card, row, anchor_x, bounds);
            });
        }
    }

    if (has_bar) {
        const UiId bar_id = ui_id("server_scrollbar");
        if (!ctx.draw) {
            if (live && ctx.click && ui_point_in(ctx.mx, ctx.my, bar.gutter)) {
                if (ctx.my < bar.track.y) {
                    top_ = std::max(0, top_ - 1);
                }
                else if (ctx.my >= bar.track.y + bar.track.h) {
                    top_ = std::min(max_top, top_ + 1);
                }
            }
            // ui_scrollbar_drag grabs on any click in the track, which only a live panel may take.
            if (live || ctx.captured_id() == bar_id) {
                const std::optional<float> t = ui_scrollbar_drag(ctx, bar_id, bar.track, bar.thumb_y, bar.thumb_h);
                if (t && bar.track.h > bar.thumb_h) {
                    top_ = std::clamp(static_cast<int>(*t * static_cast<float>(max_top) + 0.5f), 0, max_top);
                }
            }
        }
        else {
            skin_scrollbar(ctx, view, content_h, static_cast<float>(top_ * row_h), ctx.captured_id() == bar_id);
        }
    }
    if (!ctx.draw && live && ctx.wheel != 0
        && (ui_point_in(ctx.mx, ctx.my, list) || ui_point_in(ctx.mx, ctx.my, headings))) {
        top_ = std::clamp(top_ + (ctx.wheel > 0 ? -wheel_rows : wheel_rows), 0, max_top);
    }

    int status_r = content_x + content_w;
    {
        const int h = s(skin::widget_h);
        const int gap = s(16.0f);
        std::array<int, status_toggles.size()> widths{};
        int x = status_r;
        for (std::size_t i = 0; i < status_toggles.size(); ++i) {
            widths[i] = skin_lamp_toggle_width(ctx, status_toggles[i].label, chip_font);
            x -= widths[i] + gap;
        }
        status_r = x;
        for (int i = 0; i < static_cast<int>(status_toggles.size()); ++i) {
            const StatusToggle& toggle = status_toggles[i];
            bool& on = g_alpine_game_config.*toggle.setting;
            const UiRect r{x + gap, footer_y + (footer_h - h) / 2, widths[i], h};
            if (skin_lamp_toggle(ctx, r, toggle.label, on, chip_font, ui_id("server_status_toggle", i)) && live) {
                on = !on;
            }
            x = r.x + r.w;
        }
    }

    if (ctx.draw) {
        const ServerBrowserStatus status = server_browser_status();
        std::string text;
        bool progress = false;
        if (rf::gameseq_get_state() == rf::GS_MULTI_SERVER_LIST && rf::multi_join_game_is_connecting()) {
            text = "Joining...";
        }
        else if (status.activity == ServerBrowserActivity::getting_servers) {
            text = "Getting servers...";
        }
        else if (status.activity == ServerBrowserActivity::searching_lan) {
            text = "Searching the LAN...";
        }
        else if (status.activity == ServerBrowserActivity::pinging) {
            text = std::format("Pinging {}/{}", status.pinged, status.to_ping);
            progress = true;
        }
        else if (selected >= 0 && entries[selected].af) {
            text = server_clients_label(*entries[selected].af);
        }
        else if (status.tracker == TrackerResult::unreachable) {
            text = "The master server could not be reached";
        }
        else if (status.tracker == TrackerResult::timed_out) {
            text = "The master server's list did not finish arriving";
        }
        const int ty = footer_y + (footer_h - rf::gr::get_font_height(body_font)) / 2;
        const int status_span = status_r - content_x;
        const int status_w = progress ? status_span / 2 : status_span;
        skin_draw_text(content_x, ty, ui_fit_end(text, status_w - s(12.0f), body_font), body_font, skin::ink_status);
        if (progress) {
            const int bx = content_x + status_w;
            const int bw = status_span - status_w;
            const int bh = s(20.0f);
            const int by = footer_y + (footer_h - bh) / 2;
            rf::gr::set_color(0x80, 0x80, 0x80, 0xFF);
            rf::gr::rect(bx, by, bw, bh);
            rf::gr::set_color(0, 0, 0, 0xFF);
            rf::gr::rect(bx + 1, by + 1, bw - 2, bh - 2);
            const float fraction = status.to_ping > 0
                ? std::clamp(static_cast<float>(status.pinged) / static_cast<float>(status.to_ping), 0.0f, 1.0f)
                : 0.0f;
            rf::gr::set_color(0x17, 0x8C, 0x00, 0xFF);
            rf::gr::rect(bx + 1, by + 1, static_cast<int>(fraction * static_cast<float>(bw - 2)), bh - 2);
        }
    }
}
