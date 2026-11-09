#include <algorithm>
#include <chrono>
#include <format>
#include <common/utils/list-utils.h>
#include <optional>
#include <utility>
#include <patch_common/FunHook.h>
#include "multi_scoreboard.h"
#include "multi_spectate.h"
#include "../input/control_input_filter.h"
#include "../input/input.h"
#include "../multi/alpine_packets.h"
#include "../multi/endgame_votes.h"
#include "../multi/demo/demo.h"
#include "../multi/demo/demo_ui.h"
#include "../multi/multi.h"
#include "../multi/network.h"
#include "../multi/gametype.h"
#include "../multi/bagman.h"
#include "../multi/salvage.h"
#include "../multi/wipeout.h"
#include "../misc/alpine_options.h"
#include "../misc/alpine_settings.h"
#include "../misc/vote_panel.h"
#include "../misc/waypoints_utils.h"
#include "../rf/player/control_config.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/input.h"
#include "../rf/multi.h"
#include "../rf/localize.h"
#include "../rf/entity.h"
#include "../rf/gameseq.h"
#include "../rf/hud.h"
#include "../rf/level.h"
#include "../rf/os/console.h"
#include "../rf/os/timer.h"
#include "../rf/sound/sound.h"
#include "../rf/ui.h"
#include "../sound/sound.h"
#include "../os/console.h"
#include "../main/main.h"
#include "hud.h"
#include "hud_internal.h"
#include "../graphics/gr.h"
#include "../misc/player.h"
#include "../misc/settings_registry.h"

#define DEBUG_SCOREBOARD 0

namespace rf
{
static auto& draw_scoreboard = addr_as_ref<void(bool draw)>(0x00470860);
static auto& fit_scoreboard_string = addr_as_ref<String* (String* result, String::Pod str, int cx_max)>(0x00471EC0);
}

constexpr float ENTER_ANIM_MS = 100.0f;
constexpr float LEAVE_ANIM_MS = 100.0f;
constexpr int scroll_rows_per_notch = 3;
constexpr size_t column_rows = 16;
constexpr int64_t ban_confirm_delay_ms = 500;

static bool g_scoreboard_force_hide = false;
static bool g_scoreboard_visible = false;
static int64_t g_anim_ticks = 0;
static bool g_enter_anim = false;
static bool g_leave_anim = false;
static bool g_big_scoreboard = false;

// What the HUD asked for last frame. The request is consumed by the input pump, so a frame
// where the HUD never ran (menu on top, disconnect) reads as closed.
static bool g_show_requested = false;
static bool g_key_held = false;
static bool g_cursor_unhooked = false;
static bool g_toggle_armed = false;
// A button held through a re-hook must not fire or respawn when the veto lifts.
static bool g_swallow_attack = false;
static int g_pending_scroll_rows = 0;
static int g_scroll_offset = 0;

namespace
{

struct ScoreboardListView
{
    int top = 0;
    int bottom = 0;
    int scroll = 0;
    bool hover_enabled = false;
    int mouse_x = 0;
    int mouse_y = 0;
    int pinned_player_id = -1;
    rf::Player* hovered = nullptr;
};

struct ScoreboardRect
{
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    [[nodiscard]] bool contains(int px, int py) const
    {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

enum class PopoutButtonKind
{
    Mute,
    Spectate,
    Kick,
    Ban,
    ServerMuteChat,
    SwapTeam,
    ToggleSpectate,
    ToggleReady,
};

struct PopoutButton
{
    PopoutButtonKind kind = PopoutButtonKind::Mute;
    ScoreboardRect rect{};
};

// By id: the pin outlives frames, and a player can leave at any time.
struct ScoreboardPin
{
    int player_id = -1;
    int x = 0;
    int y = 0;
    bool ban_armed = false;
    int64_t ban_armed_ms = 0;
    ScoreboardRect box{};
    std::vector<PopoutButton> buttons{};
};

} // namespace

static ScoreboardPin g_pin{};
static int g_hovered_player_id = -1;

struct ScoreboardPlayerList
{
    std::vector<rf::Player*> players{};
    std::vector<size_t> divider_indices{};
};

enum class ScoreboardCategory
{
    Active,
    Bot,
    Spectator,
    Idle,
    Browser,
    // Pit-only sections, appended in top-to-bottom display order (the sort
    // comparator orders by category ordinal). In Pit every player maps to one
    // of these, bypassing the config-driven categories above.
    PitDueler,
    PitQueued,
    PitNotQueued,
};

// Section header label for a Pit category (nullptr for non-Pit categories).
static const char* pit_section_label(ScoreboardCategory cat)
{
    switch (cat) {
        case ScoreboardCategory::PitDueler:    return "DUELING";
        case ScoreboardCategory::PitQueued:    return "IN QUEUE";
        case ScoreboardCategory::PitNotQueued: return "NOT QUEUED";
        default:                               return nullptr;
    }
}

static ScoreboardCategory get_scoreboard_category(const rf::Player* player)
{
    // Pit: role comes from the replicated roster and fully overrides the local
    // scoreboard_split_* settings. Unknown/none (e.g. browsers) folds into NOT
    // QUEUED.
    if (gt_is_pit()) {
        const int role = pit_scoreboard_role_for(player);
        if (role == 0) return ScoreboardCategory::PitDueler;
        if (role == 1) return ScoreboardCategory::PitQueued;
        return ScoreboardCategory::PitNotQueued;
    }

    if (g_alpine_game_config.scoreboard_split_bots && player->is_bot) {
        return ScoreboardCategory::Bot;
    }

    if (g_alpine_game_config.scoreboard_split_spectators && player->is_spectator) {
        return ScoreboardCategory::Spectator;
    }

    if (g_alpine_game_config.scoreboard_split_idle && player_is_idle(player)) {
        return ScoreboardCategory::Idle;
    }

    if (g_alpine_game_config.scoreboard_split_browsers && player->is_browser) {
        return ScoreboardCategory::Browser;
    }

    return ScoreboardCategory::Active;
}

static std::vector<size_t> calculate_divider_indices(const std::vector<rf::Player*>& players)
{
    std::vector<size_t> divider_indices{};
    if (players.empty()) {
        return divider_indices;
    }

    // split once for all categories other than active. Pit always splits at
    // every category (its three sections are independent of the client's
    // scoreboard_split_* settings), so skip the simple path there.
    if (!gt_is_pit() && g_alpine_game_config.scoreboard_split_simple) {
        bool has_active = false;
        std::optional<size_t> first_non_active{};

        for (size_t i = 0; i < players.size(); ++i) {
            const ScoreboardCategory cat = get_scoreboard_category(players[i]);
            has_active = has_active || (cat == ScoreboardCategory::Active);

            if (cat != ScoreboardCategory::Active && !first_non_active) {
                first_non_active = i;
            }
        }

        if (has_active && first_non_active) {
            divider_indices.push_back(*first_non_active);
        }
        return divider_indices;
    }

    // split at every category
    ScoreboardCategory current = get_scoreboard_category(players.front());
    for (size_t i = 1; i < players.size(); ++i) {
        const ScoreboardCategory next = get_scoreboard_category(players[i]);
        if (next != current) {
            divider_indices.push_back(i);
            current = next;
        }
    }
    return divider_indices;
}

bool multi_scoreboard_is_visible() {
    return g_scoreboard_visible;
}

void multi_scoreboard_set_big(bool is_big)
{
    g_big_scoreboard = is_big;
}

bool multi_scoreboard_cursor_active()
{
    return g_cursor_unhooked;
}

static bool scoreboard_cursor_available()
{
    return g_alpine_game_config.scoreboard_cursor && !demo_controls_ui_is_open() && !waypoints_utils_editor_active();
}

static void scoreboard_rehook_cursor()
{
    if (!g_cursor_unhooked) {
        return;
    }
    g_cursor_unhooked = false;
    g_swallow_attack = true;
    g_pin = {};
    g_hovered_player_id = -1;
    // The mouse override is shared: the overlay that closed the scoreboard now owns it.
    if (!vote_panel_is_gameplay_overlay_active() && !demo_controls_ui_is_open()) {
        gameplay_overlay_apply_mouse(false);
    }
}

static void scoreboard_press_button(PopoutButtonKind kind)
{
    rf::Player* const target = rf::multi_find_player_by_id(static_cast<uint8_t>(g_pin.player_id));
    if (!target) {
        g_pin = {};
        return;
    }

    rf::snd_play(stock_sound_id::panel_button_click, 0, 0.0f, 1.0f);
    switch (kind) {
        case PopoutButtonKind::Mute:
            target->is_muted = !target->is_muted;
            g_pin.ban_armed = false;
            break;
        case PopoutButtonKind::Spectate:
            multi_spectate_set_target_player(target);
            g_pin.ban_armed = false;
            break;
        case PopoutButtonKind::ServerMuteChat:
            af_send_rcon_player_action(static_cast<uint8_t>(g_pin.player_id), AF_RCON_PLAYER_MUTE_CHAT);
            g_pin.ban_armed = false;
            break;
        case PopoutButtonKind::Kick:
            af_send_rcon_player_action(static_cast<uint8_t>(g_pin.player_id), AF_RCON_PLAYER_KICK);
            g_pin = {};
            break;
        case PopoutButtonKind::Ban: {
            const int64_t now_ms = timer::get_i64(1000);
            if (!g_pin.ban_armed) {
                g_pin.ban_armed = true;
                g_pin.ban_armed_ms = now_ms;
            }
            // The delay keeps a double-click from confirming its own first click.
            else if (now_ms - g_pin.ban_armed_ms >= ban_confirm_delay_ms) {
                af_send_rcon_player_action(static_cast<uint8_t>(g_pin.player_id), AF_RCON_PLAYER_BAN);
                g_pin = {};
            }
            break;
        }
        case PopoutButtonKind::SwapTeam:
            af_send_rcon_player_action(static_cast<uint8_t>(g_pin.player_id), AF_RCON_PLAYER_SWAP_TEAM);
            g_pin.ban_armed = false;
            break;
        case PopoutButtonKind::ToggleSpectate:
            af_send_rcon_player_action(
                static_cast<uint8_t>(g_pin.player_id),
                target->is_spectator ? AF_RCON_PLAYER_FORCE_UNSPECTATE : AF_RCON_PLAYER_FORCE_SPECTATE);
            g_pin.ban_armed = false;
            break;
        case PopoutButtonKind::ToggleReady:
            af_send_rcon_player_action(static_cast<uint8_t>(g_pin.player_id), AF_RCON_PLAYER_TOGGLE_READY);
            g_pin.ban_armed = false;
            break;
    }
}

static void scoreboard_handle_click()
{
    if (!rf::mouse_was_button_pressed(0) || rf::console::console_is_visible() || rf::ui::popup_is_active()) {
        return;
    }

    int x = 0, y = 0, z = 0;
    rf::mouse_get_pos(x, y, z);
    for (const PopoutButton& button : g_pin.buttons) {
        if (button.rect.contains(x, y)) {
            scoreboard_press_button(button.kind);
            return;
        }
    }
    if (g_pin.player_id >= 0 && g_pin.box.contains(x, y)) {
        return;
    }
    if (g_hovered_player_id >= 0 && g_hovered_player_id != g_pin.player_id) {
        g_pin = {};
        g_pin.player_id = g_hovered_player_id;
        g_pin.x = x;
        g_pin.y = y;
    }
    else {
        g_pin = {};
    }
}

// Runs before the state frame: a key-bound press is consumed by whichever read sees it first.
void multi_scoreboard_input()
{
    if (g_swallow_attack) {
        rf::ControlConfig* const ccp = rf::local_player ? &rf::local_player->settings.controls : nullptr;
        g_swallow_attack = ccp
            && (control_input_filter_check_pressed_unfiltered(ccp, rf::CC_ACTION_PRIMARY_ATTACK, nullptr)
                || control_input_filter_check_pressed_unfiltered(ccp, rf::CC_ACTION_SECONDARY_ATTACK, nullptr));
    }

    const rf::GameState state = rf::gameseq_get_state();
    const bool limbo = state == rf::GS_MULTI_LIMBO;
    const bool open = std::exchange(g_show_requested, false)
        && rf::is_multi
        && rf::local_player
        && !g_scoreboard_force_hide
        && (limbo || state == rf::GS_GAMEPLAY)
        && scoreboard_cursor_available();

    if (!open) {
        g_toggle_armed = false;
        g_pending_scroll_rows = 0;
        g_scroll_offset = 0;
        scoreboard_rehook_cursor();
        return;
    }

    if (limbo) {
        g_toggle_armed = false;
        g_cursor_unhooked = true;
    }
    else {
        const bool typing = rf::console::console_is_visible() || rf::multi_chat_is_say_visible()
            || rf::ui::popup_is_active();
        g_toggle_armed = (g_key_held || g_cursor_unhooked) && !typing;
        if (g_toggle_armed) {
            bool toggle_pressed = false;
            control_input_filter_check_pressed_unfiltered(
                &rf::local_player->settings.controls,
                rf::CC_ACTION_SECONDARY_ATTACK,
                &toggle_pressed);
            if (toggle_pressed) {
                if (g_cursor_unhooked) {
                    scoreboard_rehook_cursor();
                }
                else {
                    g_cursor_unhooked = true;
                }
            }
        }
    }

    if (g_cursor_unhooked) {
        // Re-assert every frame: respawns and camera changes reset mouse mode.
        gameplay_overlay_apply_mouse(true);
        if (rf::mouse_dz != 0) {
            g_pending_scroll_rows += rf::mouse_dz > 0 ? -scroll_rows_per_notch : scroll_rows_per_notch;
        }
        scoreboard_handle_click();
    }
}

static bool scoreboard_cursor_blocks_action(rf::ControlConfig* ccp, rf::ControlConfigAction action)
{
    if (!rf::local_player || ccp != &rf::local_player->settings.controls) {
        return false;
    }
    switch (action) {
        case rf::CC_ACTION_PRIMARY_ATTACK:
            return g_cursor_unhooked || g_swallow_attack;
        case rf::CC_ACTION_SECONDARY_ATTACK:
            return g_cursor_unhooked || g_swallow_attack || g_toggle_armed;
        default:
            return false;
    }
}

static int scoreboard_character_image(const rf::Player* player)
{
    const int character = player->settings.multi_character;
    const bool known = rf::is_server || player == rf::local_player || player->multi_character_known;
    if (!known || character < 0 || character >= rf::num_multi_characters) {
        return -1;
    }
    return rf::mp_characters[character].image_handle;
}

static std::vector<PopoutButtonKind> scoreboard_popout_actions(const rf::Player* player)
{
    if (player == rf::local_player) {
        return {};
    }
    std::vector<PopoutButtonKind> kinds{};
    if (!demo_playback_active()) {
        kinds.push_back(PopoutButtonKind::Mute);
    }
    // In limbo the engine owns the camera.
    if (rf::gameseq_get_state() == rf::GS_GAMEPLAY && multi_spectate_is_spectating() && !player->is_browser
        && !player->is_spectator) {
        kinds.push_back(PopoutButtonKind::Spectate);
    }
    return kinds;
}

static std::vector<PopoutButtonKind> scoreboard_popout_manage(const rf::Player* player)
{
    if (player == rf::local_player || demo_playback_active()) {
        return {};
    }
    std::vector<PopoutButtonKind> kinds{};
    if (local_rcon_command_available("kick")) {
        kinds.push_back(PopoutButtonKind::Kick);
    }
    if (!player->is_bot && local_rcon_command_available("ban")) {
        kinds.push_back(PopoutButtonKind::Ban);
    }
    if (!player->is_bot && local_rcon_command_available("sv_mute_chat")) {
        kinds.push_back(PopoutButtonKind::ServerMuteChat);
    }
    if (player->is_browser) {
        return kinds;
    }
    if (multi_game_type_is_team_type(rf::multi_get_game_type()) && local_rcon_command_available("sv_swap_team")) {
        kinds.push_back(PopoutButtonKind::SwapTeam);
    }
    if (!player->is_bot
        && local_rcon_command_available(player->is_spectator ? "sv_force_unspectate" : "sv_force_spectate")) {
        kinds.push_back(PopoutButtonKind::ToggleSpectate);
    }
    // A listen host never receives the ready prompt that sets the local flag.
    const bool pre_match = rf::is_server ? g_match_info.pre_match_active : get_local_pre_match_active();
    if (pre_match && !player->is_bot && local_rcon_command_available("sv_toggle_ready")) {
        kinds.push_back(PopoutButtonKind::ToggleReady);
    }
    return kinds;
}

static const char* scoreboard_button_label(PopoutButtonKind kind, const rf::Player* player)
{
    switch (kind) {
        case PopoutButtonKind::Mute:
            return player->is_muted ? "Unmute" : "Mute";
        case PopoutButtonKind::ServerMuteChat:
            return player_role_flags(player) & AF_PLAYER_ROLE_CHAT_MUTED ? "Server unmute chat" : "Server mute chat";
        case PopoutButtonKind::Spectate:
            return player == multi_spectate_get_target_player() ? "Spectating" : "Spectate";
        case PopoutButtonKind::Kick:
            return "Kick";
        case PopoutButtonKind::Ban:
            return g_pin.ban_armed ? "Confirm ban" : "Ban";
        case PopoutButtonKind::SwapTeam:
            return "Swap team";
        case PopoutButtonKind::ToggleSpectate:
            return player->is_spectator ? "Force unspectate" : "Force spectate";
        case PopoutButtonKind::ToggleReady:
            return player_role_flags(player) & AF_PLAYER_ROLE_READY ? "Unready" : "Ready";
    }
    return "";
}

static bool scoreboard_button_is_active(PopoutButtonKind kind, const rf::Player* player)
{
    switch (kind) {
        case PopoutButtonKind::Mute:
            return player->is_muted;
        case PopoutButtonKind::ServerMuteChat:
            return (player_role_flags(player) & AF_PLAYER_ROLE_CHAT_MUTED) != 0;
        case PopoutButtonKind::Spectate:
            return player == multi_spectate_get_target_player();
        case PopoutButtonKind::Ban:
            return g_pin.ban_armed;
        case PopoutButtonKind::ToggleSpectate:
            return player->is_spectator;
        case PopoutButtonKind::ToggleReady:
            return (player_role_flags(player) & AF_PLAYER_ROLE_READY) != 0;
        default:
            return false;
    }
}

static int scoreboard_button_icon(PopoutButtonKind kind)
{
    static const int mute_bm = rf::bm::load("afsbi_btn_mute.tga", -1, true);
    static const int kick_bm = rf::bm::load("afsbi_btn_kick.tga", -1, true);
    static const int ban_bm = rf::bm::load("afsbi_btn_ban.tga", -1, true);
    static const int swap_bm = rf::bm::load("afsbi_btn_swap.tga", -1, true);
    static const int spec_bm = rf::bm::load("afsbi_btn_spec.tga", -1, true);
    static const int ready_bm = rf::bm::load("afsbi_btn_ready.tga", -1, true);
    switch (kind) {
        case PopoutButtonKind::Mute:
        case PopoutButtonKind::ServerMuteChat:
            return mute_bm;
        case PopoutButtonKind::Kick:
            return kick_bm;
        case PopoutButtonKind::Ban:
            return ban_bm;
        case PopoutButtonKind::SwapTeam:
            return swap_bm;
        case PopoutButtonKind::Spectate:
        case PopoutButtonKind::ToggleSpectate:
            return spec_bm;
        case PopoutButtonKind::ToggleReady:
            return ready_bm;
    }
    return -1;
}

static std::string scoreboard_player_labels(const rf::Player* player)
{
    const uint8_t roles = player_role_flags(player);
    std::vector<std::string_view> labels;
    if (roles & AF_PLAYER_ROLE_SERVER_HOST) {
        labels.push_back("Server Host");
    }
    if (roles & AF_PLAYER_ROLE_RCON_HOLDER) {
        labels.push_back("Rcon Holder");
    }
    if (player->is_spectator) {
        labels.push_back("Spectator");
    }
    if (player->is_browser) {
        labels.push_back("Browser");
    }
    if (player->is_bot) {
        labels.push_back("Bot");
    }

    std::string text;
    for (const std::string_view label : labels) {
        if (!text.empty()) {
            text += " \x95 ";
        }
        text += label;
    }
    return text;
}

static void draw_scoreboard_player_popout(
    rf::Player* player,
    rf::NetGameType game_type,
    int anchor_x,
    int anchor_y,
    int mouse_x,
    int mouse_y,
    bool pinned,
    float scale)
{
    const PlayerStatsNew* stats = static_cast<PlayerStatsNew*>(player->stats);
    // A stock server never sends the PF stats packet, so these fields would be zero or partial.
    const bool has_server_stats = rf::is_server || player->received_pf_status.has_value();

    std::vector<std::pair<std::string, std::string>> rows;
    if (game_type != rf::NG_TYPE_RUN) {
        const float kd_ratio = static_cast<float>(stats->num_kills)
            / static_cast<float>(std::max<int>(stats->num_deaths, 1));
        rows.emplace_back("K/D ratio", std::format("{:.2f}", kd_ratio));
    }
    if (has_server_stats) {
        rows.emplace_back("Streak", std::format("{} (best {})", stats->current_streak, stats->max_streak));
        const int accuracy = rf::is_server ? stats->calc_accuracy_percent() : stats->reported_accuracy;
        rows.emplace_back("Accuracy", std::format("{}%", accuracy));
    }
    // Pinned, the Mute button says this.
    if (player->is_muted && !pinned) {
        rows.emplace_back("Muted", "Yes");
    }

    std::string title = player->name;
    gr_fit_string(title, static_cast<int>(180 * scale));

    const std::string player_labels = scoreboard_player_labels(player);
    const int label_font = hud_get_small_font();
    const int labels_h = player_labels.empty() ? 0 : rf::gr::get_font_height(label_font);
    const int labels_w = player_labels.empty() ? 0 : rf::gr::get_string_size(player_labels, label_font).first;

    int label_w = 0;
    int value_w = 0;
    for (const auto& [label, value] : rows) {
        label_w = std::max(label_w, rf::gr::get_string_size(label, -1).first);
        value_w = std::max(value_w, rf::gr::get_string_size(value, -1).first);
    }

    const int font_h = rf::gr::get_font_height(-1);
    const int padding = static_cast<int>(6 * scale);
    const int column_gap = static_cast<int>(12 * scale);
    const int title_gap = static_cast<int>(4 * scale);
    const int title_w = rf::gr::get_string_size(title, -1).first;

    const int image = scoreboard_character_image(player);
    const float image_scale = 0.2f * scale;
    int image_w = 0;
    int image_h = 0;
    if (image >= 0) {
        rf::bm::get_dimensions(image, &image_w, &image_h);
        image_w = static_cast<int>(image_w * image_scale);
        image_h = static_cast<int>(image_h * image_scale);
    }
    const int rows_w = rows.empty() ? 0 : label_w + column_gap + value_w;
    const int left_w = std::max({title_w, labels_w, image_w, rows_w});
    const int left_h = font_h + labels_h + title_gap + (image_w > 0 ? image_h + title_gap : 0)
        + static_cast<int>(rows.size()) * font_h;

    const std::vector<PopoutButtonKind> actions =
        pinned ? scoreboard_popout_actions(player) : std::vector<PopoutButtonKind>{};
    const std::vector<PopoutButtonKind> manage =
        pinned ? scoreboard_popout_manage(player) : std::vector<PopoutButtonKind>{};
    const int button_h = static_cast<int>(18 * scale);
    const int button_gap = static_cast<int>(3 * scale);
    const int heading_h = rf::gr::get_font_height(label_font);
    const auto section_h = [&](const std::vector<PopoutButtonKind>& kinds) {
        return kinds.empty() ? 0 : heading_h + static_cast<int>(kinds.size()) * (button_h + button_gap);
    };
    const int right_h = section_h(actions) + (actions.empty() || manage.empty() ? 0 : title_gap) + section_h(manage);
    // Sized for the longest label so the buttons never move when one changes.
    const int button_w = button_h + button_gap * 2 + rf::gr::get_string_size("Server unmute chat", -1).first;
    const int right_w = right_h > 0 ? column_gap + button_w : 0;

    const int box_w = padding * 2 + left_w + right_w;
    const int box_h = padding * 2 + std::max(left_h, right_h);

    const int cursor_gap = static_cast<int>(16 * scale);
    int box_x = anchor_x + cursor_gap;
    if (box_x + box_w > rf::gr::clip_width()) {
        box_x = anchor_x - cursor_gap - box_w;
    }
    box_x = std::max(box_x, 0);
    const int box_y = std::max(std::min(anchor_y, rf::gr::clip_height() - box_h), 0);

    rf::gr::set_color(0, 0, 0, 0xE0);
    rf::gr::rect(box_x, box_y, box_w, box_h);
    rf::gr::set_color(0xFF, 0xFF, 0xFF, 0x80);
    hud_rect_border(box_x, box_y, box_w, box_h, std::max(1, static_cast<int>(scale)));

    int cur_y = box_y + padding;
    const int label_x = box_x + padding;
    if (player == rf::local_player) {
        rf::gr::set_color(0xFF, 0xFF, 0x80, 0xFF);
    }
    else {
        rf::gr::set_color(0xFF, 0xFF, 0xFF, 0xFF);
    }
    rf::gr::string(label_x, cur_y, title.c_str());
    cur_y += font_h;
    if (!player_labels.empty()) {
        rf::gr::set_color(0xB0, 0xB0, 0xB0, 0xFF);
        rf::gr::string(label_x, cur_y, player_labels.c_str(), label_font);
        cur_y += labels_h;
    }
    cur_y += title_gap;

    if (image >= 0) {
        rf::gr::set_color(0xFF, 0xFF, 0xFF, 0xFF);
        hud_scaled_bitmap(image, label_x, cur_y, image_scale);
        cur_y += image_h + title_gap;
    }

    const int value_x = label_x + label_w + column_gap;
    for (const auto& [label, value] : rows) {
        rf::gr::set_color(0xB0, 0xB0, 0xB0, 0xFF);
        rf::gr::string(label_x, cur_y, label.c_str());
        rf::gr::set_color(0xFF, 0xFF, 0xFF, 0xFF);
        rf::gr::string(value_x, cur_y, value.c_str());
        cur_y += font_h;
    }

    if (!pinned) {
        return;
    }

    g_pin.box = {box_x, box_y, box_w, box_h};
    g_pin.buttons.clear();

    const int button_x = label_x + left_w + column_gap;
    int button_y = box_y + padding;
    const auto draw_section = [&](const char* heading, const std::vector<PopoutButtonKind>& kinds) {
        if (kinds.empty()) {
            return;
        }
        rf::gr::set_color(0xB0, 0xB0, 0xB0, 0xFF);
        rf::gr::string(button_x, button_y, heading, label_font);
        button_y += heading_h;
        for (const PopoutButtonKind kind : kinds) {
            const ScoreboardRect rect{button_x, button_y, button_w, button_h};
            const int shade = rect.contains(mouse_x, mouse_y) ? 0x70 : 0x40;
            rf::gr::set_color(scoreboard_button_is_active(kind, player) ? 0xA0 : shade, shade, shade, 0xFF);
            rf::gr::rect(rect.x, rect.y, rect.w, rect.h);
            rf::gr::set_color(0xFF, 0xFF, 0xFF, 0xFF);
            const int icon = scoreboard_button_icon(kind);
            int icon_w = 0, icon_h = 0;
            rf::bm::get_dimensions(icon, &icon_w, &icon_h);
            const float icon_scale = static_cast<float>(button_h) / static_cast<float>(std::max(icon_w, 1));
            hud_scaled_bitmap(icon, rect.x, rect.y, icon_scale);
            rf::gr::string(
                rect.x + button_h + button_gap,
                rect.y + (button_h - font_h) / 2,
                scoreboard_button_label(kind, player));
            g_pin.buttons.push_back({kind, rect});
            button_y += button_h + button_gap;
        }
    };
    draw_section("Actions", actions);
    if (!actions.empty()) {
        button_y += title_gap;
    }
    draw_section("Manage", manage);
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
        rf::String server_info =
            rf::String::format("{} (IP: {})", rf::netgame.name, rf::netgame.server_addr);
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
            else if (game_type == rf::NG_TYPE_TBAG) {
                red_score = bagman_get_red_team_score();
                blue_score = bagman_get_blue_team_score();
            }
            else if (game_type == rf::NG_TYPE_SAL || game_type == rf::NG_TYPE_SCAV) {
                // Placeholder art: Salvage and Scavenger borrow the CTF flag icons.
                static int hud_flag_red_bm = rf::bm::load("hud_flag_red.tga", -1, true);
                static int hud_flag_blue_bm = rf::bm::load("hud_flag_blue.tga", -1, true);
                int flag_bm_w, flag_bm_h;
                rf::bm::get_dimensions(hud_flag_red_bm, &flag_bm_w, &flag_bm_h);
                rf::gr::bitmap(hud_flag_red_bm, x + w * 2 / 6 - flag_bm_w / 2, cur_y);
                rf::gr::bitmap(hud_flag_blue_bm, x + w * 4 / 6 - flag_bm_w / 2, cur_y);
                red_score = salvage_get_red_team_score();
                blue_score = salvage_get_blue_team_score();
            }
            else if (game_type == rf::NG_TYPE_WO) {
                static int hud_flag_red_bm = rf::bm::load("hud_flag_red.tga", -1, true);
                static int hud_flag_blue_bm = rf::bm::load("hud_flag_blue.tga", -1, true);
                int flag_bm_w, flag_bm_h;
                rf::bm::get_dimensions(hud_flag_red_bm, &flag_bm_w, &flag_bm_h);
                rf::gr::bitmap(hud_flag_red_bm, x + w * 2 / 6 - flag_bm_w / 2, cur_y);
                rf::gr::bitmap(hud_flag_blue_bm, x + w * 4 / 6 - flag_bm_w / 2, cur_y);
                red_score = wipeout_get_red_team_score();
                blue_score = wipeout_get_blue_team_score();
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

int draw_scoreboard_players(
    const ScoreboardPlayerList& player_list,
    int x,
    int y,
    int w,
    float scale,
    rf::NetGameType game_type,
    bool dry_run = false,
    ScoreboardListView* view = nullptr)
{
    int initial_y = y;
    int font_h = rf::gr::get_font_height(-1);
    const int row_h = font_h + (scale == 1.0f ? 3 : 0);
    const int row_spacing = font_h;
    const int divider_spacing = row_spacing / 4;
    const int divider_height = std::max(1, static_cast<int>(scale));

    // Pit replaces the plain divider line with a small section header per group.
    const bool is_pit = gt_is_pit();
    const int section_font = hud_get_small_font();
    const int section_font_h = rf::gr::get_font_height(section_font);

    int status_w = static_cast<int>(12 * scale);
    int score_w = static_cast<int>((game_type == rf::NG_TYPE_RUN ? 63 : 50) * scale);
    bool show_kd = game_type != rf::NG_TYPE_RUN;
    int kd_w = show_kd ? static_cast<int>(70 * scale) : 0;
    // Assists are only meaningful where kills are. Kept separate from show_kd so the two
    // can diverge later.
    bool show_assists = game_type != rf::NG_TYPE_RUN;
    int assists_w = show_assists ? static_cast<int>(35 * scale) : 0;
    const bool show_caps = game_type == rf::NG_TYPE_CTF || game_type == rf::NG_TYPE_SAL
        || game_type == rf::NG_TYPE_SCAV;
    int caps_w = show_caps ? static_cast<int>(45 * scale) : 0;
    const auto& server_info = get_af_server_info();
    bool saving_enabled = server_info.has_value() && server_info->saving_enabled;
    bool show_loads = game_type == rf::NG_TYPE_RUN && saving_enabled;
    int loads_w = show_loads ? static_cast<int>(55 * scale) : 0;
    int ping_w = static_cast<int>(35 * scale);
    int name_w = w - status_w - score_w - kd_w - assists_w - caps_w - loads_w - ping_w;

    // Every column but the name has a fixed width, so the assists column is paid for out of
    // the name. Give it back rather than squeeze names to nothing on a narrow panel (team
    // game types split the table in two, and CTF spends another column on caps).
    const int min_name_w = static_cast<int>(80 * scale);
    if (show_assists && name_w < min_name_w) {
        name_w += assists_w;
        assists_w = 0;
        show_assists = false;
    }

    int status_x = x;
    int name_x = status_x + status_w;
    int score_x = name_x + name_w;
    int kd_x = score_x + score_w;
    int assists_x = kd_x + kd_w;
    int caps_x = assists_x + assists_w;
    int loads_x = caps_x + caps_w;
    int ping_x = loads_x + loads_w;

    // Draw list header
    if (!dry_run) {
        rf::gr::set_color(0xFF, 0xFF, 0xFF, 0xFF);
        rf::gr::string(name_x, y, rf::strings::player);
        if (game_type == rf::NG_TYPE_RUN) {
            rf::gr::string(score_x, y, "Deaths");
        } else {
            rf::gr::string(score_x, y, rf::strings::score); // Note: RF uses "Frags"
        }
        if (show_kd) {
            rf::gr::string(kd_x, y, "K/D");
        }
        if (show_assists) {
            rf::gr::string(assists_x, y, "Asst");
        }
        if (show_caps) {
            rf::gr::string(caps_x, y, rf::strings::caps);
        }
        if (show_loads) {
            rf::gr::string(loads_x, y, "Loads");
        }
        rf::gr::string(ping_x, y, rf::strings::ping, -1);
    }

    y += font_h + 8;
    if (view) {
        y -= view->scroll;
    }
    const auto in_view = [&](int item_y, int item_h) {
        return !dry_run && (!view || (item_y >= view->top && item_y + item_h <= view->bottom));
    };

    rf::Player* red_flag_player = rf::multi_ctf_get_red_flag_player();
    rf::Player* blue_flag_player = rf::multi_ctf_get_blue_flag_player();

    // Draw the list
    size_t next_divider = 0;
    for (size_t i = 0; i < player_list.players.size(); ++i) {
        rf::Player* player = player_list.players[i];

        const bool at_divider = next_divider < player_list.divider_indices.size()
            && i == player_list.divider_indices[next_divider];

        if (is_pit) {
            // Pit: draw a section header before the very first row and at each
            // category boundary (every divider, since Pit splits at every
            // category). Empty sections produce no divider, hence no header.
            // Heights are added identically in dry_run and real draw so the
            // measurement pass stays in sync with the actual layout.
            if (i == 0 || at_divider) {
                if (at_divider) {
                    y += divider_spacing; // gap above the header
                    ++next_divider;
                }
                if (const char* label = pit_section_label(get_scoreboard_category(player))) {
                    if (in_view(y, section_font_h)) {
                        rf::gr::set_color(0xFF, 0xFF, 0xFF, 0xB0);
                        rf::gr::string(x, y, label, section_font);
                    }
                    y += section_font_h + divider_spacing;
                }
            }
        }
        else if (at_divider) {
            y += divider_spacing - divider_height;
            if (in_view(y, divider_height)) {
                rf::gr::set_color(0xFF, 0xFF, 0xFF, 0x80);
                rf::gr::rect(x, y, w, divider_height);
            }
            y += divider_spacing;
            ++next_divider;
        }

        if (in_view(y, font_h)) {
            const bool hovered = view && view->hover_enabled
                && ScoreboardRect{x, y, w, row_h}.contains(view->mouse_x, view->mouse_y);
            if (hovered) {
                view->hovered = player;
            }
            const bool row_pinned = view && player->net_data && player->net_data->player_id == view->pinned_player_id;
            if (hovered || row_pinned) {
                rf::gr::set_color(0xFF, 0xFF, 0xFF, row_pinned ? 0x50 : 0x30);
                rf::gr::rect(x, y, w, row_h);
            }

            static const int green_bm = rf::bm::load("afsbi_spawned.tga", -1, true);
            static const int red_bm = rf::bm::load("afsbi_dead.tga", -1, true);
            static const int browser_bm = rf::bm::load("afsbi_brow.tga", -1, true);
            static const int spectator_bm = rf::bm::load("afsbi_spec.tga", -1, true);
            static const int idle_bm = rf::bm::load("afsbi_idle.tga", -1, true);
            static const int hud_micro_flag_red_bm =
                rf::bm::load("hud_microflag_red.tga", -1, true);
            static const int hud_micro_flag_blue_bm =
                rf::bm::load("hud_microflag_blue.tga", -1, true);
            static const int hud_micro_flag_sal_bm =
                rf::bm::load("hud_microflag_sal.vbm", -1, true);
            static const int hud_micro_flag_scav_bm =
                rf::bm::load("hud_microflag_scav.tga", -1, true);

        const bool is_spawned = !rf::player_is_dead(player)
            && !rf::player_is_dying(player);
        const int status_bm = std::invoke([&] {
            if (player->is_browser) {
                return browser_bm;
            } else if (player->is_spectator) {
                return spectator_bm;
            } else if ((player->is_bot
                && player->is_spawn_disabled
                && !is_spawned)
                || player_is_idle(player)) {
                return idle_bm;
            } else if (salvage_player_is_carrier(player)) {
                return gt_is_scavenger() ? hud_micro_flag_scav_bm : hud_micro_flag_sal_bm;
            } else if (player == red_flag_player) {
                return hud_micro_flag_red_bm;
            } else if (player == blue_flag_player) {
                return hud_micro_flag_blue_bm;
            } else {
                return is_spawned ? green_bm : red_bm;
            }
        });

            int bm_w = 0, bm_h = 0;
            rf::bm::get_dimensions(status_bm, &bm_w, &bm_h);
            const int offset = std::lround(
                (static_cast<float>(font_h)
                - static_cast<float>(bm_h)
                * scale)
                / 2.f
            );
            rf::gr::set_color(0xFF, 0xFF, 0xFF, 0xFF);
            hud_scaled_bitmap(
                status_bm,
                status_x,
                y + offset,
                scale
            );

            const bool is_local_player = player == rf::player_list;
            if (is_local_player) {
                rf::gr::set_color(0xFF, 0xFF, 0x80, 0xFF);
            } else {
                rf::gr::set_color(0xFF, 0xFF, 0xFF, 0xFF);
            }

            std::string player_name_stripped = player->name;
            const auto [space_w, space_h] = rf::gr::get_char_size(' ', -1);
            const bool is_bot = player->is_bot;
            const int bot_font = hud_get_small_font();
            if (is_bot) {
                const auto [bot_w, bot_h] = rf::gr::get_string_size(" BOT", bot_font);
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
                int bot_y = y + (rf::gr::get_font_height(-1) - rf::gr::get_font_height(bot_font)) / 2;
                rf::gr::string(rf::gr::current_string_x, bot_y, " BOT", bot_font);
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
            int num_assists = 999;
            int caps_or_loads = 999;
            int ping = 9999;
#else
            const PlayerStatsNew* stats = static_cast<PlayerStatsNew*>(player->stats);
            int score = stats->score;
            int num_kills = stats->num_kills;
            int num_deaths = stats->num_deaths;
            int num_assists = stats->num_assists;
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

            if (show_assists) {
                auto assists_str = std::to_string(num_assists);
                rf::gr::string(assists_x, y, assists_str.c_str());
            }

            if (show_caps) {
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

        y += row_h;
    }

    return y - initial_y;
}

static std::string scoreboard_cursor_hint(bool overflows)
{
    const rf::GameState state = rf::gameseq_get_state();
    const bool limbo = state == rf::GS_MULTI_LIMBO;
    // The between-levels screens draw this table directly, with no cursor to offer.
    if ((!limbo && state != rf::GS_GAMEPLAY) || !scoreboard_cursor_available()
        || !g_alpine_game_config.scoreboard_cursor_hint) {
        return {};
    }
    if (limbo) {
        return overflows ? "Mouse wheel to scroll" : "";
    }
    const std::string toggle_bind = get_action_bind_name(rf::CC_ACTION_SECONDARY_ATTACK);
    if (g_cursor_unhooked) {
        return std::format(
            "{} to release cursor, click a player for actions{}",
            toggle_bind,
            overflows ? ", mouse wheel to scroll" : "");
    }
    return g_key_held
        ? std::format("{} to use cursor", toggle_bind)
        : std::format("Hold {} and press {} to use cursor", get_action_bind_name(rf::CC_ACTION_MP_STATS), toggle_bind);
}

ScoreboardPlayerList filter_and_sort_players(const std::optional<int> team_id)
{
    ScoreboardPlayerList player_list{};
    player_list.players.reserve(32);

    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        // The demo viewer is not part of the recorded match - no phantom row
        if (demo_playback_active() && &player == rf::local_player) {
            continue;
        }
        // A recording listen-server host must not list its own virtual recorder
        if (player.is_observer()) {
            continue;
        }
        if (!team_id || player.team == team_id.value()) {
            player_list.players.push_back(&player);
        }
    }

    std::ranges::sort(
        player_list.players,
        [] (const rf::Player* const player_1, const rf::Player* const player_2) {
            const ScoreboardCategory category_1 = get_scoreboard_category(player_1);
            const ScoreboardCategory category_2 = get_scoreboard_category(player_2);

            if (category_1 != category_2) {
                return category_1 < category_2;
            }

            // Pit IN QUEUE section: order by queue position (front of queue on
            // top). DUELING / NOT QUEUED keep the standard sort below. Ties or
            // unknown order (0) fall through to the standard comparison.
            if (category_1 == ScoreboardCategory::PitQueued) {
                const int order_1 = pit_scoreboard_order_for(player_1);
                const int order_2 = pit_scoreboard_order_for(player_2);
                if (order_1 != order_2 && order_1 != 0 && order_2 != 0) {
                    return order_1 < order_2;
                }
            }

            const rf::NetGameType game_type = rf::multi_get_game_type();
            if (game_type == rf::NG_TYPE_RUN) {
                const PlayerStatsNew* const stats_1 =
                    static_cast<PlayerStatsNew*>(player_1->stats);
                const PlayerStatsNew* const stats_2 =
                    static_cast<PlayerStatsNew*>(player_2->stats);
                if (stats_1->num_deaths != stats_2->num_deaths) {
                    return stats_1->num_deaths < stats_2->num_deaths;
                } else if (player_1->stats->caps != player_2->stats->caps) {
                    return player_1->stats->caps < player_2->stats->caps;
                }
            } else {
                if (player_1->stats->score != player_2->stats->score) {
                    return player_1->stats->score > player_2->stats->score;
                }
            }

            // Sort players before bots, and both before browsers.
            if (player_1->is_human_player) {
                return player_2->is_bot || player_2->is_browser;
            } else {
                return player_1->is_bot && player_2->is_browser;
            }
        }
    );

    player_list.divider_indices = calculate_divider_indices(player_list.players);

    return player_list;
}

void draw_scoreboard_internal_new(bool draw) {
    if (g_scoreboard_force_hide || !draw) {
        return;
    }

    const rf::NetGameType game_type = rf::multi_get_game_type();
    ScoreboardPlayerList left_players{}, right_players{};
    bool split_columns = multi_game_type_is_team_type(game_type);
#if DEBUG_SCOREBOARD
    if (split_columns) {
        for (int i = 0; i < 16; ++i) {
            left_players.players.push_back(rf::local_player);
        }
        for (int i = 0; i < 16; ++i) {
            right_players.players.push_back(rf::local_player);
        }
    } else {
        for (int i = 0; i < 32; ++i) {
            left_players.players.push_back(rf::local_player);
        }
    }
    {
#else
    if (split_columns) {
        left_players = filter_and_sort_players({rf::TEAM_RED});
        right_players = filter_and_sort_players({rf::TEAM_BLUE});
    } else {
        left_players = filter_and_sort_players({});
#endif
        if (left_players.players.size() > column_rows) {
            const size_t left_rows = std::max(column_rows, (left_players.players.size() + 1) / 2);
            const auto overflow_start = left_players.players.begin() + left_rows;
            right_players.players.assign(overflow_start, left_players.players.end());
            left_players.players.erase(overflow_start, left_players.players.end());

            left_players.divider_indices = calculate_divider_indices(left_players.players);
            right_players.divider_indices = calculate_divider_indices(right_players.players);

            split_columns = true;
        }
    }

    // Animation
    float anim_progress = 1.0f;
    float progress_w = 1.0f;
    float progress_h = 1.0f;
    if (g_alpine_game_config.scoreboard_anim) {
        const int64_t anim_delta = timer::get_i64(1000) - g_anim_ticks;
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

    const int min_space_w = static_cast<int>(rf::gr::clip_width() / 1280.f * 40.f);
    const int max_scoreboard_w = rf::gr::clip_width() - min_space_w;

    int w = 0;
    float scale = 0.f;
    if (g_big_scoreboard) {
        // Note: fit_scoreboard_string does not support providing font by argument so 
        // default font must be changed
        rf::gr::set_default_font(hud_get_default_font_name(true));
        w = std::min(split_columns ? 1400 : 900, max_scoreboard_w);
        scale = 2.f;
    } else {
        w = std::min(split_columns ? 700 : 450, max_scoreboard_w);
        scale = 1.f;
    }

    int left_padding = static_cast<int>(10 * scale);
    int right_padding = static_cast<int>(10 * scale);
    int middle_padding = static_cast<int>(15 * scale);
    int top_padding = static_cast<int>(10 * scale);
    int bottom_padding = static_cast<int>(5 * scale);
    int hdr_h = draw_scoreboard_header(0, 0, w, game_type, true);
    int left_players_h = draw_scoreboard_players(left_players, 0, 0, 0, scale, game_type, true);
    int right_players_h = draw_scoreboard_players(right_players, 0, 0, 0, scale, game_type, true);

    const int font_h = rf::gr::get_font_height(-1);
    const int list_header_h = font_h + 8;
    const int row_h = font_h + (scale == 1.0f ? 3 : 0);
    const int rows_h = std::max(left_players_h, right_players_h) - list_header_h;
    const int chrome_h = top_padding + hdr_h + list_header_h + bottom_padding;
    const int max_scoreboard_h =
        rf::gr::clip_height() - static_cast<int>(rf::gr::clip_height() / 720.f * 40.f);
    const int hint_font = hud_get_small_font();
    const bool overflows = chrome_h + rows_h > max_scoreboard_h;
    const std::string cursor_hint = scoreboard_cursor_hint(overflows);
    const int hint_h = cursor_hint.empty() ? 0 : rf::gr::get_font_height(hint_font) + 4;
    const int viewport_h = overflows ? std::max(row_h, max_scoreboard_h - chrome_h - hint_h) : rows_h;
    const int max_scroll = std::max(rows_h - viewport_h, 0);
    g_scroll_offset = std::clamp(g_scroll_offset + g_pending_scroll_rows * row_h, 0, max_scroll);
    g_pending_scroll_rows = 0;
    int h = chrome_h + viewport_h + hint_h;

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

    ScoreboardListView view{};
    view.top = y + list_header_h;
    view.bottom = view.top + viewport_h;
    view.scroll = g_scroll_offset;
    if (g_cursor_unhooked) {
        int mouse_z = 0;
        rf::mouse_get_pos(view.mouse_x, view.mouse_y, mouse_z);
        view.hover_enabled = g_pin.player_id < 0 || !g_pin.box.contains(view.mouse_x, view.mouse_y);
    }
    rf::Player* const pinned_player = g_pin.player_id >= 0
        ? rf::multi_find_player_by_id(static_cast<uint8_t>(g_pin.player_id))
        : nullptr;
    if (!pinned_player) {
        g_pin = {};
    }
    view.pinned_player_id = g_pin.player_id;

    if (split_columns) {
        int table_w = (w - left_padding - middle_padding - right_padding) / 2;
        draw_scoreboard_players(left_players, x + left_padding, y, table_w, scale, game_type, false, &view);
        draw_scoreboard_players(
            right_players, x + left_padding + table_w + middle_padding, y, table_w, scale, game_type, false, &view);
    } else {
        int table_w = w - left_padding - right_padding;
        draw_scoreboard_players(left_players, x + left_padding, y, table_w, scale, game_type, false, &view);
    }

    if (max_scroll > 0) {
        const int bar_w = std::max(2, static_cast<int>(3 * scale));
        const int bar_x = x + w - (right_padding + bar_w) / 2;
        const int thumb_h = std::max(row_h, viewport_h * viewport_h / rows_h);
        const int thumb_y = view.top + (viewport_h - thumb_h) * g_scroll_offset / max_scroll;
        rf::gr::set_color(0xFF, 0xFF, 0xFF, 0x40);
        rf::gr::rect(bar_x, view.top, bar_w, viewport_h);
        rf::gr::set_color(0xFF, 0xFF, 0xFF, 0xC0);
        rf::gr::rect(bar_x, thumb_y, bar_w, thumb_h);
    }

    // Key release changes the text a frame before the close animation hides the panel.
    if (!cursor_hint.empty() && !g_leave_anim) {
        rf::gr::set_color(0xFF, 0xFF, 0xFF, 0xB0);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, x + w / 2, view.bottom + 2, cursor_hint.c_str(), hint_font);
    }

    g_hovered_player_id = view.hovered && view.hovered->net_data ? view.hovered->net_data->player_id : -1;
    if (pinned_player) {
        draw_scoreboard_player_popout(
            pinned_player, game_type, g_pin.x, g_pin.y, view.mouse_x, view.mouse_y, true, scale);
    }
    else if (view.hovered) {
        draw_scoreboard_player_popout(
            view.hovered, game_type, view.mouse_x, view.mouse_y, view.mouse_x, view.mouse_y, false, scale);
    }

    // Restore rfpc-medium as default font
    if (g_big_scoreboard) {
        rf::gr::set_default_font("rfpc-medium.vf");
    }
}

FunHook<void(bool)> draw_scoreboard_internal_hook{0x00470880, draw_scoreboard_internal_new};

ConsoleCommand2 ui_scoreboard_spectators_cmd{
    "ui_scoreboard_split_spectators",
    [] {
        setting_console_set("scoreboard_split_spectators", "toggle");
        rf::console::print(
            "Scoreboard spectator separation is {}",
            g_alpine_game_config.scoreboard_split_spectators ? "enabled" : "disabled"
        );
    },
    "Toggle whether spectators are grouped separately on the scoreboard",
    "ui_scoreboard_split_spectators",
};

ConsoleCommand2 ui_scoreboard_bots_cmd{
    "ui_scoreboard_split_bots",
    [] {
        setting_console_set("scoreboard_split_bots", "toggle");
        rf::console::print(
            "Scoreboard bot separation is {}",
            g_alpine_game_config.scoreboard_split_bots ? "enabled" : "disabled"
        );
    },
    "Toggle whether bots are grouped separately on the scoreboard",
    "ui_scoreboard_split_bots",
};

ConsoleCommand2 ui_scoreboard_browsers_cmd{
    "ui_scoreboard_split_browsers",
    [] {
        setting_console_set("scoreboard_split_browsers", "toggle");
        rf::console::print(
            "Scoreboard browser separation is {}",
            g_alpine_game_config.scoreboard_split_browsers ? "enabled" : "disabled"
        );
    },
    "Toggle whether browsers are grouped separately on the scoreboard",
    "ui_scoreboard_split_browsers",
};

ConsoleCommand2 ui_scoreboard_idle_cmd{
    "ui_scoreboard_split_idle",
    [] {
        setting_console_set("scoreboard_split_idle", "toggle");
        rf::console::print(
            "Scoreboard idle player separation is {}",
            g_alpine_game_config.scoreboard_split_idle ? "enabled" : "disabled"
        );
    },
    "Toggle whether idle players are grouped separately on the scoreboard",
    "ui_scoreboard_split_idle",
};

ConsoleCommand2 ui_scoreboard_cursor_cmd{
    "ui_scoreboard_cursor",
    [] {
        setting_console_set("scoreboard_cursor", "toggle");
        rf::console::print(
            "Scoreboard cursor is {}",
            g_alpine_game_config.scoreboard_cursor ? "enabled" : "disabled"
        );
    },
    "Toggle the scoreboard cursor, which allows hovering over players and scrolling the list",
    "ui_scoreboard_cursor",
};

ConsoleCommand2 ui_scoreboard_cursor_hint_cmd{
    "ui_scoreboard_cursor_hint",
    [] {
        setting_console_set("scoreboard_cursor_hint", "toggle");
        rf::console::print(
            "Scoreboard cursor hint text is {}",
            g_alpine_game_config.scoreboard_cursor_hint ? "enabled" : "disabled"
        );
    },
    "Toggle the scoreboard cursor hint text",
    "ui_scoreboard_cursor_hint",
};

ConsoleCommand2 ui_scoreboard_simple_cmd{
    "ui_scoreboard_simplesplit",
    [] {
        setting_console_set("scoreboard_split_simple", "toggle");
        rf::console::print(
            "Simple scoreboard separation is {}",
            g_alpine_game_config.scoreboard_split_simple ? "enabled" : "disabled"
        );
    },
    "Toggle whether each scoreboard grouping is displayed individually",
    "ui_scoreboard_simplesplit",
};

void scoreboard_maybe_render(bool show_scoreboard, bool key_held)
{
    g_show_requested = show_scoreboard;
    g_key_held = key_held;

    if (g_alpine_game_config.scoreboard_anim) {
        if (!g_scoreboard_visible && show_scoreboard) {
            g_enter_anim = true;
            g_leave_anim = false;
            g_anim_ticks = timer::get_i64(1000);
            g_scoreboard_visible = true;
        }
        if (g_scoreboard_visible && !show_scoreboard && !g_leave_anim) {
            g_enter_anim = false;
            g_leave_anim = true;
            g_anim_ticks = timer::get_i64(1000);
        }
    }
    else {
        g_scoreboard_visible = show_scoreboard;
        g_leave_anim = false;
    }

    if (g_scoreboard_visible) {
        rf::draw_scoreboard(true);
    }
}

void multi_scoreboard_apply_patch()
{
    draw_scoreboard_internal_hook.install();
    control_input_filter_add_veto(&scoreboard_cursor_blocks_action);
    ui_scoreboard_spectators_cmd.register_cmd();
    ui_scoreboard_bots_cmd.register_cmd();
    ui_scoreboard_browsers_cmd.register_cmd();
    ui_scoreboard_idle_cmd.register_cmd();
    ui_scoreboard_cursor_cmd.register_cmd();
    ui_scoreboard_cursor_hint_cmd.register_cmd();
    ui_scoreboard_simple_cmd.register_cmd();
}

void multi_scoreboard_set_hidden(bool hidden)
{
    g_scoreboard_force_hide = hidden;
}
