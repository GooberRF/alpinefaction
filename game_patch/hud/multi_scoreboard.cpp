#include <algorithm>
#include <chrono>
#include <format>
#include <common/utils/list-utils.h>
#include <optional>
#include <patch_common/FunHook.h>
#include "multi_scoreboard.h"
#include "../input/input.h"
#include "../multi/endgame_votes.h"
#include "../multi/multi.h"
#include "../multi/gametype.h"
#include "../misc/alpine_options.h"
#include "../misc/alpine_settings.h"
#include "../rf/player/control_config.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/multi.h"
#include "../rf/localize.h"
#include "../rf/entity.h"
#include "../rf/gameseq.h"
#include "../rf/hud.h"
#include "../rf/level.h"
#include "../rf/os/timer.h"
#include "../main/main.h"
#include "hud_internal.h"
#include "../graphics/gr.h"
#include "../misc/player.h"

#define DEBUG_SCOREBOARD 0

namespace rf
{
static auto& draw_scoreboard = addr_as_ref<void(bool draw)>(0x00470860);
static auto& fit_scoreboard_string = addr_as_ref<String* (String* result, String::Pod str, int cx_max)>(0x00471EC0);
}

constexpr float ENTER_ANIM_MS = 100.0f;
constexpr float LEAVE_ANIM_MS = 100.0f;

static bool g_scoreboard_force_hide = false;
static bool g_scoreboard_visible = false;
static unsigned g_anim_ticks = 0;
static bool g_enter_anim = false;
static bool g_leave_anim = false;
static bool g_big_scoreboard = false;

bool multi_scoreboard_is_visible() {
    return g_scoreboard_visible;
}

void multi_scoreboard_set_big(bool is_big)
{
    g_big_scoreboard = is_big;
}

int draw_scoreboard_header(int x, int y, int w, rf::NetGameType game_type, bool dry_run = false)
{
    // Draw RF logo
    int x_center = x + w / 2;
    int cur_y = y;
    if (!dry_run) {
        rf::gr::set_color(0xFF, 0xFF, 0xFF, 0xFF);
        static int score_rflogo_bm =
            rf::bm::load(g_alpine_game_config.af_branding ? "score_aflogo.tga" : "score_rflogo.tga", -1, false);
        rf::gr::bitmap(score_rflogo_bm, x_center - 170, cur_y);
    }
    cur_y += 30;

    // Draw Game Type name
    if (!dry_run) {
        const std::string game_info = std::format(
            "{} \x95 {}/{} PLAYING",
            multi_game_type_name_upper(game_type),
            multi_num_spawned_players(),
            rf::multi_num_players()
        );
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, x_center, cur_y, game_info.c_str());
    }
    int font_h = rf::gr::get_font_height(-1);
    cur_y += font_h + 8;

    // Draw endgame voting text
    int y_vote = static_cast<int>(rf::gr::screen_height() * 0.25f);
    if (!dry_run && g_player_can_endgame_vote) {
        std::string vote_yes_key_text =
            get_action_bind_name(get_af_control(rf::AlpineControlConfigAction::AF_ACTION_VOTE_YES));

        std::string vote_no_key_text =
            get_action_bind_name(get_af_control(rf::AlpineControlConfigAction::AF_ACTION_VOTE_NO));

        std::string endgame_vote_text = "Did you enjoy this map?\n\n" + vote_yes_key_text + " for yes\n" + vote_no_key_text + " for no";

        rf::gr::string_aligned(rf::gr::ALIGN_LEFT, 8, y_vote, endgame_vote_text.c_str(), 0);
    }

    // Draw level
    if (!dry_run) {
        rf::gr::set_color(0xB0, 0xB0, 0xB0, 0xFF);
        std::string level_info = std::format(
            "{1} \x95 {0:%Y} ({2}) by {3}",
            std::chrono::sys_seconds{
                std::chrono::seconds{rf::level.level_timestamp}
            },
            rf::level.name,
            rf::level.filename,
            rf::level.author
        );
        gr_fit_string(level_info, w - 20);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, x_center, cur_y, level_info.c_str());
    }
    cur_y += font_h + 3;

    // Draw server info
    if (!dry_run) {
        char ip_addr_buf[64];
        rf::net_addr_to_string(ip_addr_buf, sizeof(ip_addr_buf), rf::netgame.server_addr);
        auto server_info = rf::String::format("{} ({})", rf::netgame.name, ip_addr_buf);
        rf::String server_info_stripped;
        rf::fit_scoreboard_string(&server_info_stripped, server_info, w - 20); // Note: this destroys input string
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, x_center, cur_y, server_info_stripped);
    }
    cur_y += font_h + 8;

    // Draw team scores
    if (multi_game_type_is_team_type(game_type)) {
        if (!dry_run) {
            unsigned red_score = 0, blue_score = 0;
            if (game_type == rf::NG_TYPE_CTF) {
                static int hud_flag_red_bm = rf::bm::load("hud_flag_red.tga", -1, true);
                static int hud_flag_blue_bm = rf::bm::load("hud_flag_blue.tga", -1, true);
                int flag_bm_w, flag_bm_h;
                rf::bm::get_dimensions(hud_flag_red_bm, &flag_bm_w, &flag_bm_h);
                rf::gr::bitmap(hud_flag_red_bm, x + w * 2 / 6 - flag_bm_w / 2, cur_y);
                rf::gr::bitmap(hud_flag_blue_bm, x + w * 4 / 6 - flag_bm_w / 2, cur_y);
                red_score = rf::multi_ctf_get_red_team_score();
                blue_score = rf::multi_ctf_get_blue_team_score();
            }
            else if (game_type == rf::NG_TYPE_TEAMDM) {
                red_score = rf::multi_tdm_get_red_team_score();
                blue_score = rf::multi_tdm_get_blue_team_score();
            }
            else if (game_type == rf::NG_TYPE_KOTH || game_type == rf::NG_TYPE_DC) { // todo: new HUD icons for koth/dc
                static int hud_flag_red_bm = rf::bm::load("hud_flag_red.tga", -1, true);
                static int hud_flag_blue_bm = rf::bm::load("hud_flag_blue.tga", -1, true);
                int flag_bm_w, flag_bm_h;
                rf::bm::get_dimensions(hud_flag_red_bm, &flag_bm_w, &flag_bm_h);
                rf::gr::bitmap(hud_flag_red_bm, x + w * 2 / 6 - flag_bm_w / 2, cur_y);
                rf::gr::bitmap(hud_flag_blue_bm, x + w * 4 / 6 - flag_bm_w / 2, cur_y);
                red_score = multi_koth_get_red_team_score();
                blue_score = multi_koth_get_blue_team_score();
            }
            else if (game_type == rf::NG_TYPE_REV || game_type == rf::NG_TYPE_ESC) {
                static int hud_flag_red_bm = rf::bm::load("hud_flag_red.tga", -1, true);
                static int hud_flag_blue_bm = rf::bm::load("hud_flag_blue.tga", -1, true);
                int flag_bm_w, flag_bm_h;
                rf::bm::get_dimensions(hud_flag_red_bm, &flag_bm_w, &flag_bm_h);
                rf::gr::bitmap(hud_flag_red_bm, x + w * 2 / 6 - flag_bm_w / 2, cur_y);
                rf::gr::bitmap(hud_flag_blue_bm, x + w * 4 / 6 - flag_bm_w / 2, cur_y);
            }
            // draw scores
            if (game_type != rf::NG_TYPE_REV && game_type != rf::NG_TYPE_ESC) {
                rf::gr::set_color(0xD0, 0x20, 0x20, 0xFF);
                int team_scores_font = rf::scoreboard_big_font;
                auto red_score_str = std::to_string(red_score);
                rf::gr::string_aligned(rf::gr::ALIGN_CENTER, x + w * 1 / 6, cur_y + 10, red_score_str.c_str(), team_scores_font);
                rf::gr::set_color(0x20, 0x20, 0xD0, 0xFF);
                auto blue_score_str = std::to_string(blue_score);
                rf::gr::string_aligned(rf::gr::ALIGN_CENTER, x + w * 5 / 6, cur_y + 10, blue_score_str.c_str(), team_scores_font);
            }
        }

        cur_y += 60;
    }

    return cur_y - y;
}

int draw_scoreboard_players(const std::vector<rf::Player*>& players, int x, int y, int w, float scale,
    rf::NetGameType game_type, bool dry_run = false)
{
    int initial_y = y;
    int font_h = rf::gr::get_font_height(-1);

    int status_w = static_cast<int>(12 * scale);
    int score_w = static_cast<int>(50 * scale);
    bool show_kd = game_type != rf::NG_TYPE_RUN;
    int kd_w = show_kd ? static_cast<int>(70 * scale) : 0;
    int caps_w = game_type == rf::NG_TYPE_CTF ? static_cast<int>(45 * scale) : 0;
    const auto& server_info = get_af_server_info();
    bool saving_enabled = server_info.has_value() && server_info->saving_enabled;
    bool show_loads = game_type == rf::NG_TYPE_RUN && saving_enabled;
    int loads_w = show_loads ? static_cast<int>(55 * scale) : 0;
    int ping_w = static_cast<int>(35 * scale);
    int name_w = w - status_w - score_w - kd_w - caps_w - loads_w - ping_w;

    int status_x = x;
    int name_x = status_x + status_w;
    int score_x = name_x + name_w;
    int kd_x = score_x + score_w;
    int caps_x = kd_x + kd_w;
    int loads_x = caps_x + caps_w;
    int ping_x = loads_x + loads_w;

    // Draw list header
    if (!dry_run) {
        rf::gr::set_color(0xFF, 0xFF, 0xFF, 0xFF);
        rf::gr::string(name_x, y, rf::strings::player);
        rf::gr::string(score_x, y, rf::strings::score); // Note: RF uses "Frags"
        if (show_kd) {
            rf::gr::string(kd_x, y, "K/D");
        }
        if (game_type == rf::NG_TYPE_CTF) {
            rf::gr::string(caps_x, y, rf::strings::caps);
        }
        if (show_loads) {
            rf::gr::string(loads_x, y, "Loads");
        }
        rf::gr::string(ping_x, y, rf::strings::ping, -1);
    }

    y += font_h + 8;

    rf::Player* red_flag_player = rf::multi_ctf_get_red_flag_player();
    rf::Player* blue_flag_player = rf::multi_ctf_get_blue_flag_player();

    // Draw the list
    for (rf::Player* player : players) {
        if (!dry_run) {
            static const int green_bm = rf::bm::load("afsbi_spawned.tga", -1, true);
            static const int red_bm = rf::bm::load("afsbi_dead.tga", -1, true);
            static const int browser_bm = rf::bm::load("afsbi_brow.tga", -1, true);
            static const int spectator_bm = rf::bm::load("afsbi_spec.tga", -1, true);
            static const int idle_bm = rf::bm::load("afsbi_idle.tga", -1, true);
            static const int hud_micro_flag_red_bm =
                rf::bm::load("hud_microflag_red.tga", -1, true);
            static const int hud_micro_flag_blue_bm =
                rf::bm::load("hud_microflag_blue.tga", -1, true);

            const auto& pdata = get_player_additional_data(player);
            const int status_bm = std::invoke([&] {
                if (pdata.is_browser()) {
                    return browser_bm;
                } else if (pdata.is_spectator()) {
                    return spectator_bm;
                } else if ((pdata.is_spawn_disabled_bot()
                    && rf::player_is_dead(player))
                    || is_player_idle(player)) {
                    return idle_bm;
                } else {
                    if (player == red_flag_player) {
                        return hud_micro_flag_red_bm;
                    } else if (player == blue_flag_player) {
                        return hud_micro_flag_blue_bm;
                    } else {
                        const rf::Entity* const entity =
                            rf::entity_from_handle(player->entity_handle);
                        return entity ? green_bm : red_bm;
                    }
                }
            });

            rf::gr::set_color(0xFF, 0xFF, 0xFF, 0xFF);
            hud_scaled_bitmap(status_bm, status_x, static_cast<int>(y + 2 * scale), scale);

            const bool is_local_player = player == rf::player_list;
            if (is_local_player) {
                rf::gr::set_color(0xFF, 0xFF, 0x80, 0xFF);
            } else {
                rf::gr::set_color(0xFF, 0xFF, 0xFF, 0xFF);
            }

            std::string player_name_stripped = player->name;
            const auto [space_w, space_h] = rf::gr::get_char_size(' ', -1);
            const bool is_bot = pdata.is_bot();
            if (is_bot) {
                const auto [bot_w, bot_h] = rf::gr::get_string_size(" bot", -1);
                gr_fit_string(
                    player_name_stripped,
                    name_w - bot_w - space_w
                );
            } else {
                gr_fit_string(
                    player_name_stripped,
                    name_w - space_w
                );
            }

            if (is_bot) {
                rf::gr::string(name_x, y, player_name_stripped.c_str());
                rf::gr::set_color(255, 250, 205, 255);
                rf::gr::string(rf::gr::current_string_x, y, " bot");
                if (is_local_player) {
                    rf::gr::set_color(0xFF, 0xFF, 0x80, 0xFF);
                } else {
                    rf::gr::set_color(0xFF, 0xFF, 0xFF, 0xFF);
                }
            } else {
                rf::gr::string(name_x, y, player_name_stripped.c_str());
            }

#if DEBUG_SCOREBOARD
            int score = 999;
            int num_kills = 999;
            int num_deaths = 999;
            int caps_or_loads = 999;
            int ping = 9999;
#else
            const PlayerStatsNew* stats = static_cast<PlayerStatsNew*>(player->stats);
            int score = stats->score;
            int num_kills = stats->num_kills;
            int num_deaths = stats->num_deaths;
            int caps_or_loads = stats->caps;
            int ping = player->net_data ? player->net_data->ping : 0;
#endif

            int displayed_score = game_type == rf::NG_TYPE_RUN ? num_deaths : score;
            auto score_str = std::to_string(displayed_score);
            rf::gr::string(score_x, y, score_str.c_str());

            if (show_kd) {
                auto kills_deaths_str = std::format("{}/{}", num_kills, num_deaths);
                rf::gr::string(kd_x, y, kills_deaths_str.c_str());
            }

            if (game_type == rf::NG_TYPE_CTF) {
                auto caps_str = std::to_string(caps_or_loads);
                rf::gr::string(caps_x, y, caps_str.c_str());
            }

            if (show_loads) {
                auto loads_str = std::to_string(caps_or_loads);
                rf::gr::string(loads_x, y, loads_str.c_str());
            }

            auto ping_str = std::to_string(ping);
            rf::gr::string(ping_x, y, ping_str.c_str());
        }

        y += font_h + (scale == 1.0f ? 3 : 0);
    }

    return y - initial_y;
}

void filter_and_sort_players(
    std::vector<rf::Player*>& players,
    const std::optional<int> team_id
) {
    players.clear();
    players.reserve(32);

    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (!team_id || player.team == team_id.value()) {
            players.push_back(&player);
        }
    }

    std::ranges::sort(
        players,
        [] (const rf::Player* const player_1, const rf::Player* const player_2) {
            if (player_1->stats->score != player_2->stats->score) {
                return player_1->stats->score > player_2->stats->score;
            }
            const auto& pdata_1 = get_player_additional_data(player_1);
            const auto& pdata_2 = get_player_additional_data(player_2);
            // Sort players before bots, and both before browsers.
            if (pdata_1.is_proper_player()) {
                return pdata_2.is_bot() || pdata_2.is_browser();
            } else {
                return pdata_1.is_bot() && pdata_2.is_browser();
            }
        }
    );
}

void draw_scoreboard_internal_new(bool draw) {
    if (g_scoreboard_force_hide || !draw) {
        return;
    }

    const rf::NetGameType game_type = rf::multi_get_game_type();
    std::vector<rf::Player*> left_players{}, right_players{};
    bool split_columns = multi_game_type_is_team_type(game_type);
#if DEBUG_SCOREBOARD
    if (split_columns) {
        for (int i = 0; i < 16; ++i) {
            left_players.push_back(rf::local_player);
        }
        for (int i = 0; i < 16; ++i) {
            right_players.push_back(rf::local_player);
        }
    } else {
        for (int i = 0; i < 32; ++i) {
            left_players.push_back(rf::local_player);
        }
    }
    {
#else
    if (split_columns) {
        filter_and_sort_players(left_players, {rf::TEAM_RED});
        filter_and_sort_players(right_players, {rf::TEAM_BLUE});
    } else {
        filter_and_sort_players(left_players, {});
#endif
        if (left_players.size() > 16) {
            const auto overflow_start = left_players.begin() + 16;
            right_players.assign(overflow_start, left_players.end());
            left_players.erase(overflow_start, left_players.end());
            split_columns = true;
        }
    }

    // Animation
    float anim_progress = 1.0f;
    float progress_w = 1.0f;
    float progress_h = 1.0f;
    if (g_alpine_game_config.scoreboard_anim) {
        unsigned anim_delta = rf::timer_get(1000) - g_anim_ticks;
        if (g_enter_anim) {
            anim_progress = anim_delta / ENTER_ANIM_MS;
        } else if (g_leave_anim) {
            anim_progress = (LEAVE_ANIM_MS - anim_delta) / LEAVE_ANIM_MS;
        }

        if (g_leave_anim && anim_progress <= 0.0f) {
            g_scoreboard_visible = false;
            return;
        }

        progress_w = anim_progress * 2.0f;
        progress_h = (anim_progress - 0.5f) * 2.0f;

        progress_w = std::clamp(progress_w, 0.1f, 1.0f);
        progress_h = std::clamp(progress_h, 0.1f, 1.0f);
    }

    int w;
    float scale;
    // Note: fit_scoreboard_string does not support providing font by argument so default font must be changed
    if (g_big_scoreboard) {
        rf::gr::set_default_font(hud_get_default_font_name(true));
        w = std::min(!split_columns ? 900 : 1400, rf::gr::clip_width() - 10);
        scale = 2.0f;
    } else {
        w = std::min(!split_columns ? 450 : 700, rf::gr::clip_width() - 10);
        scale = 1.0f;
    }

    int left_padding = static_cast<int>(10 * scale);
    int right_padding = static_cast<int>(10 * scale);
    int middle_padding = static_cast<int>(15 * scale);
    int top_padding = static_cast<int>(10 * scale);
    int bottom_padding = static_cast<int>(5 * scale);
    int hdr_h = draw_scoreboard_header(0, 0, w, game_type, true);
    int left_players_h = draw_scoreboard_players(left_players, 0, 0, 0, scale, game_type, true);
    int right_players_h = draw_scoreboard_players(right_players, 0, 0, 0, scale, game_type, true);
    int h = top_padding + hdr_h + std::max(left_players_h, right_players_h) + bottom_padding;

    // Draw background
    w = static_cast<int>(progress_w * w);
    h = static_cast<int>(progress_h * h);
    int x = (static_cast<int>(rf::gr::clip_width()) - w) / 2;
    int y = (static_cast<int>(rf::gr::clip_height()) - h) / 2;
    rf::gr::set_color(0, 0, 0, 0x80);
    rf::gr::rect(x, y, w, h);
    y += top_padding;

    if (progress_h < 1.0f || progress_w < 1.0f) {
        // Restore rfpc-medium as default font
        if (g_big_scoreboard) {
            rf::gr::set_default_font("rfpc-medium.vf");
        }
        return;
    }

    y += draw_scoreboard_header(x, y, w, game_type);
    if (split_columns) {
        int table_w = (w - left_padding - middle_padding - right_padding) / 2;
        draw_scoreboard_players(left_players, x + left_padding, y, table_w, scale, game_type);
        draw_scoreboard_players(right_players, x + left_padding + table_w + middle_padding, y, table_w, scale, game_type);
    } else {
        int table_w = w - left_padding - right_padding;
        draw_scoreboard_players(left_players, x + left_padding, y, table_w, scale, game_type);
    }

    // Restore rfpc-medium as default font
    if (g_big_scoreboard) {
        rf::gr::set_default_font("rfpc-medium.vf");
    }
}

FunHook<void(bool)> draw_scoreboard_internal_hook{0x00470880, draw_scoreboard_internal_new};

void scoreboard_maybe_render(bool show_scoreboard)
{
    if (g_alpine_game_config.scoreboard_anim) {
        if (!g_scoreboard_visible && show_scoreboard) {
            g_enter_anim = true;
            g_leave_anim = false;
            g_anim_ticks = rf::timer_get(1000);
            g_scoreboard_visible = true;
        }
        if (g_scoreboard_visible && !show_scoreboard && !g_leave_anim) {
            g_enter_anim = false;
            g_leave_anim = true;
            g_anim_ticks = rf::timer_get(1000);
        }
    }
    else {
        g_scoreboard_visible = show_scoreboard;
    }

    if (g_scoreboard_visible) {
        rf::draw_scoreboard(true);
    }
}

void multi_scoreboard_apply_patch()
{
    draw_scoreboard_internal_hook.install();
}

void multi_scoreboard_set_hidden(bool hidden)
{
    g_scoreboard_force_hide = hidden;
}
