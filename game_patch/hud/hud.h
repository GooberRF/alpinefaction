#pragma once

#include "../rf/input.h"
#include "../os/os.h"
#include <variant>
#include <vector>
#include <optional>
#include <string>
#include <string_view>

namespace rf
{
    struct Player;
}

struct af_gungame_order_entry;

enum class ChatMenuType : int
{
    None,
    Comms,
    Taunts,
    Commands,
    Spectate
};

// The af_server_msg HUD-notification path casts a wire byte to this enum, so
// new values must be appended at the END (keep in sync with the allowlist in
// af_process_server_msg_packet).
enum class HudNotificationType : int
{
    None = 0,
    ReadyUp,        // local only
    BagCarrier,     // local only
    Round,          // round-based gametypes (countdown, round start, milestones)
    GametypeHelp,   // local only
    Queue,          // local only, persistent (Pit duel queue overlay)
    GunGame,        // Gun Game weapon-progression notifications
    Rampage,        // big center-screen callout (renders in its own slot)
    Generic,        // standard slot (under the chat box)
    GenericBig,     // big center-screen slot
};

void hud_notification_show(std::string text, int duration_seconds, HudNotificationType type, bool fade_on_expire);
void hud_notification_remove(HudNotificationType type, bool instant);

// Returns true if HUD should be hidden (cl_hud off, or cinematic spectate freelook)
bool is_hud_effectively_hidden();

void hud_apply_patches();
int hud_get_small_font();
int hud_get_default_font();
int hud_get_large_font();
bool hud_weapons_is_double_ammo();
void draw_hud_vote_notification(std::string vote_type);
void remove_hud_vote_notification();
void stop_draw_respawn_timer_notification();
void draw_respawn_timer_notification(bool can_respawn, bool force_respawn, int spawn_delay);
void draw_hud_ready_notification(bool draw);
void set_local_pre_match_active(bool set_active);
bool get_local_pre_match_active();
void apply_ready_prompt_state(uint8_t state); // 0/1/2 tri-state ready-prompt signal
void set_local_pit_queue_state(bool queued, bool dueler, int pos, int total, bool spectate);
void reset_local_pit_queue_state();
// Replicated Pit roster (af_pit_roster) — client-side storage + scoreboard getters.
void reset_local_pit_roster();
void set_local_pit_roster_entry(uint8_t player_id, uint8_t role, uint8_t order);
int pit_scoreboard_role_for(const rf::Player* player); // 0=dueler,1=queued,2=not_queued,-1=unknown
int pit_scoreboard_order_for(const rf::Player* player); // 1-based queue position, 0 if unknown/not-queued
// Client per-frame: auto-enter freelook spectate for a queued Pit player when
// the server flags it. Call from the per-frame tick (not the HUD render path)
// so it runs even with the HUD hidden.
void hud_pit_queue_auto_spectate();

void set_local_gungame_order(const af_gungame_order_entry* entries, uint8_t count);
void reset_local_gungame_order();
void gungame_client_do_frame(); // per-frame level-up watcher (client only)
void multi_hud_level_init();
void multi_hud_on_local_spawn();
void multi_hud_reset_gametype_help();
void multi_hud_reset_run_gt_timer(bool triggered_by_respawn_key);
void multi_hud_update_timer_color();
void toggle_chat_menu(ChatMenuType state);
bool get_chat_menu_is_active();
void hud_render_draw_chat_menu();
void chat_menu_action_handler(rf::Key key);
void build_local_player_spectators_strings();
std::string_view multi_hud_get_random_taunt_message();
bool multi_hud_send_taunt_chat_message(std::string_view taunt_text);

extern int g_multi_hud_cp_strip_y;
