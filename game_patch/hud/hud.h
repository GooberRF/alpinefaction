#pragma once

#include "../rf/input.h"

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
int hud_get_chat_font();
int hud_get_console_font();
int hud_get_health_font();
int hud_get_ammo_font();
int hud_get_timer_font();
int hud_get_fps_font();
int hud_get_ping_font();
int hud_get_messages_font();
int hud_get_scoreboard_font();
int hud_get_large_font();
bool hud_weapons_is_double_ammo();
void draw_hud_vote_notification(std::string vote_type);
void remove_hud_vote_notification();
void draw_hud_ready_notification(bool draw);
void set_local_pre_match_active(bool set_active);
void build_chat_menu_comms_messages();
void toggle_chat_menu(ChatMenuType state);
bool get_chat_menu_is_active();
void hud_render_draw_chat_menu();
void chat_menu_action_handler(rf::Key key);
