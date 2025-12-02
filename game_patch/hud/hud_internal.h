#pragma once

#include <string>
#include "../rf/gr/gr.h"
#include "../rf/input.h"

namespace rf
{
    struct HudPoint;
}

enum class ChatMenuListName : int
{
    Null,
    Commands,
    Taunts,
    Radio,
    General,
    Express,
    Compliment,
    Respond,
    AttackDefend,
    Enemy,
    Timing,
    Powerup,
    Flag,
    Map,
    Intimidation,
    Mockery,
    Celebration,
    Dismissiveness,
    Bravado,
    Derision,
    Casual,
    RandomFunny,
    SpectateFreelookDefault,
    SpectateFreelookModifier
};

enum class ChatMenuListType : int
{
    Basic,
    Map,
    TeamMode,
    CTF
};

struct ChatMenuElement
{
    bool is_menu = false;
    ChatMenuListName menu = ChatMenuListName::Null;
    ChatMenuListType type = ChatMenuListType::Basic;
    std::string display_string = "";
    std::string long_string = "";
};

struct ChatMenuList
{
    std::string display_string = "";
    ChatMenuListType type = ChatMenuListType::Basic;
    std::vector<ChatMenuElement> elements;
};

extern bool g_pre_match_active;

void hud_status_apply_patches();
void hud_status_set_big(bool is_big);
void hud_personas_apply_patches();
void hud_personas_set_big(bool is_big);
void hud_weapons_apply_patches();
void hud_weapons_set_big(bool is_big);
void weapon_select_apply_patches();
void weapon_select_set_big(bool is_big);
void multi_hud_chat_apply_patches();
void multi_hud_chat_set_big(bool is_big);
void multi_hud_apply_patches();
void multi_hud_set_big(bool is_big);
void hud_scaled_bitmap(int bmh, int x, int y, float scale, rf::gr::Mode mode = rf::gr::bitmap_clamp_mode);
void hud_preload_scaled_bitmap(int bmh);
void hud_rect_border(int x, int y, int w, int h, int border, rf::gr::Mode state = rf::gr::rect_mode);
std::string hud_fit_string(std::string_view str, int max_w, int* str_w_out, int font_id);
rf::HudPoint hud_scale_coords(rf::HudPoint pt, float scale);
const char* hud_get_default_font_name(bool big);
const char* hud_get_bold_font_name(bool big);
void scoreboard_maybe_render(bool show_scoreboard);
int hud_transform_value(int val, int old_max, int new_max);
int hud_scale_value(int val, int max, float scale);
void message_log_apply_patch();
void hud_world_apply_patch();
