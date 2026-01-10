#pragma once

#include "../rf/input.h"
#include "../os/os.h"
#include <variant>
#include <vector>
#include <optional>

enum class ChatMenuType : int
{
    None,
    Comms,
    Taunts,
    Commands,
    Spectate
};

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
void multi_hud_level_init();
void multi_hud_on_local_spawn();
void multi_hud_reset_run_gt_timer(bool triggered_by_respawn_key);
void multi_hud_update_timer_color();
void toggle_chat_menu(ChatMenuType state);
bool get_chat_menu_is_active();
void hud_render_draw_chat_menu();
void chat_menu_action_handler(rf::Key key);
void build_local_player_spectators_strings();
