#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <xlog/xlog.h>
#include "../multi/multi.h"
#include "../input/input.h"
#include "../rf/input.h"
#include "../rf/hud.h"
#include "../rf/level.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../rf/os/frametime.h"
#include "../rf/os/console.h"
#include "../rf/os/os.h"
#include "../misc/player.h"
#include "../main/main.h"
#include "../graphics/gr.h"
#include "../misc/alpine_options.h"
#include "../misc/alpine_settings.h"
#include "../sound/sound.h"
#include "../os/console.h"
#include "hud_internal.h"
#include "hud.h"

static bool g_big_team_scores_hud = false;
constexpr bool g_debug_team_scores_hud = false;
static bool g_draw_vote_notification = false;
static std::string g_active_vote_type = "";
static bool g_draw_ready_notification = false;
bool g_pre_match_active = false;
static std::string time_left_string_format = "";
static int time_left_string_x_pos_offset = 135;
static int time_left_string_y_pos_offset = 21;
static std::tuple time_left_string_color = {0, 255, 0, 255};

// Radio messages
static const ChatMenuList radio_messages_menu{
    .display_string = "RADIO MESSAGES",
    .type = ChatMenuListType::Basic,
    .elements = {
        {true, ChatMenuListName::General, ChatMenuListType::Basic, "General"},
        {true, ChatMenuListName::Flag, ChatMenuListType::CTF, "Flag"},
        {true, ChatMenuListName::AttackDefend, ChatMenuListType::TeamMode, "Attack/Defend"},
        {true, ChatMenuListName::Enemy, ChatMenuListType::TeamMode, "Enemy"},
        {true, ChatMenuListName::Timing, ChatMenuListType::TeamMode, "Timing"},
        {true, ChatMenuListName::Powerup, ChatMenuListType::TeamMode, "Powerup"},
        {true, ChatMenuListName::Map, ChatMenuListType::Map, "Map"}
    }
};

// General
static const ChatMenuList general_menu{
    .display_string = "GENERAL",
    .type = ChatMenuListType::Basic,
    .elements = {
        {true, ChatMenuListName::Express, ChatMenuListType::Basic, "Express (Public)"},
        {true, ChatMenuListName::Compliment, ChatMenuListType::TeamMode, "Compliment"},
        {true, ChatMenuListName::Respond, ChatMenuListType::TeamMode, "Respond"}
    }
};

// Express (public)
static const ChatMenuList express_menu{
    .display_string = "EXPRESS (Public)",
    .type = ChatMenuListType::Basic,
    .elements = {
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Hello", "Hello"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Goodbye", "Goodbye"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Oops...", "Oops..."},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "RED FACTION!", "RED FACTION!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Quiet!", "Quiet!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Modder", "Modder"}
    }
};

// Compliment
static const ChatMenuList compliment_menu{
    .display_string = "COMPLIMENT",
    .type = ChatMenuListType::TeamMode,
    .elements = {
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "Good job!", "Good job!"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "Well played!", "Well played!"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "Nice frag!", "Nice frag!"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "You're on fire!", "You're on fire!"}
    }
};

// Respond
static const ChatMenuList respond_menu{
    .display_string = "RESPOND",
    .type = ChatMenuListType::TeamMode,
    .elements = {
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "Yes", "Yes"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "No", "No"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "I don't know", "I don't know"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "Thanks", "Thanks"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "Any time", "Any time"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "Got it", "Got it"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "Sorry", "Sorry"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "Wait", "Wait"}
    }
};

// Attack/defend
static const ChatMenuList attack_defend_menu{
    .display_string = "ATTACK/DEFEND",
    .type = ChatMenuListType::TeamMode,
    .elements = {
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "Attack coming high", "Attack incoming from high"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "Attack coming mid", "Attack incoming from mid"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "Attack coming low", "Attack incoming from low"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "Defend!", "Defend!"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "Cover me!", "Cover me!"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "Wait for my signal", "Wait for my signal"}
    }
};

// Enemy menu
static const ChatMenuList enemy_menu{
    .display_string = "ENEMY",
    .type = ChatMenuListType::TeamMode,
    .elements = {
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "Enemy going high", "Enemy is going high"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "Enemy going mid", "Enemy is going mid"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "Enemy going low", "Enemy is going low"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "Enemy down", "Enemy is down"}
    }
};

// Timing menu
static const ChatMenuList timing_menu{
    .display_string = "TIMING",
    .type = ChatMenuListType::TeamMode,
    .elements = {
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "Damage Amp soon", "Damage Amplifier is respawning soon"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "Fusion soon", "Fusion is respawning soon"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "Super Armor soon", "Super Armor is respawning soon"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "Super Health soon", "Super Health is respawning soon"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "Invuln soon", "Invulnerability is respawning soon"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode, "Rail soon", "Rail Driver is respawning soon"}
    }
};

// Powerup menu
static const ChatMenuList powerup_menu{
    .display_string = "POWERUP",
    .type = ChatMenuListType::TeamMode,
    .elements = {
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode,"Damage Amp up!", "Damage Amplifier is up!"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode,"Fusion up!", "Fusion is up!"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode,"Super Armor up!", "Super Armor is up!"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode,"Super Health up!", "Super Health is up!"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode,"Invuln up!", "Invulnerability is up!"},
        {false, ChatMenuListName::Null, ChatMenuListType::TeamMode,"Rail up!", "Rail Driver is up!"}
    }
};

// Flag
static const ChatMenuList ctf_menu{
    .display_string = "FLAG",
    .type = ChatMenuListType::CTF,
    .elements = {
        {false, ChatMenuListName::Null, ChatMenuListType::CTF, "Where enemy flag?", "Where's the enemy flag?"},
        {false, ChatMenuListName::Null, ChatMenuListType::CTF, "Where our flag?", "Where's our flag?"},
        {false, ChatMenuListName::Null, ChatMenuListType::CTF, "Take this flag", "Take the flag from me"},
        {false, ChatMenuListName::Null, ChatMenuListType::CTF, "Give me the flag", "Give me the flag"},
        {false, ChatMenuListName::Null, ChatMenuListType::CTF, "I'll get the flag", "I'll get the flag"},
        {false, ChatMenuListName::Null, ChatMenuListType::CTF, "Get our flag!", "Get our flag!"},
        {false, ChatMenuListName::Null, ChatMenuListType::CTF, "Our flag is secure", "Our flag is secure"}
    }
};

// Taunt menu
static const ChatMenuList taunt_menu{
    .display_string = "TAUNTS",
    .type = ChatMenuListType::Basic,
    .elements = {
        {true, ChatMenuListName::Intimidation, ChatMenuListType::Basic, "Intimidation"},
        {true, ChatMenuListName::Mockery, ChatMenuListType::Basic, "Mockery"},
        {true, ChatMenuListName::Celebration, ChatMenuListType::Basic, "Celebration"},
        {true, ChatMenuListName::Dismissiveness, ChatMenuListType::Basic, "Dismissive"},
        {true, ChatMenuListName::Bravado, ChatMenuListType::Basic, "Bravado"},
        {true, ChatMenuListName::Derision, ChatMenuListType::Basic, "Derision"},
        {true, ChatMenuListName::Casual, ChatMenuListType::Basic, "Casual"},
        {true, ChatMenuListName::RandomFunny, ChatMenuListType::Basic, "Random"}
    }
};

// Intimidation
static const ChatMenuList intimidation_menu{
    .display_string = "INTIMIDATION",
    .type = ChatMenuListType::Basic,
    .elements = {
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Commence beatdown!", "Commence beatdown!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Rest in pieces.", "Rest in pieces."},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "You make a nice target.", "You make a nice target."},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Squeegee time!", "Squeegee time!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Goodbye Mr. Gibs!", "Goodbye Mr. Gibs!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Have a seat, son!", "Have a seat, son!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Your move, creep!", "Your move, creep!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Messy.", "Messy."}
    }
};

// Mockery
static const ChatMenuList mockery_menu{
    .display_string = "MOCKERY",
    .type = ChatMenuListType::Basic,
    .elements = {
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Just a flesh wound.", "Just a flesh wound."},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Look, a jigsaw puzzle!", "Look, a jigsaw puzzle!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Llama!", "Llama!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Woohoo!", "Woohoo!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Chump!", "Chump!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Banned.", "Banned."},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Aww, does it hurt?", "Aww, does it hurt?"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Pathetic!", "Pathetic!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Got death smarts.", "Got death smarts."}
    }
};

// Celebration
static const ChatMenuList celebration_menu{
    .display_string = "CELEBRATION",
    .type = ChatMenuListType::Basic,
    .elements = {
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Aw yeah!", "Aw yeah!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Damn, I'm good.", "Damn, I'm good."},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Ka-ching!", "Ka-ching!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Frag-o-licious!", "Frag-o-licious!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Me red, you dead!", "Me red, you dead!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Boom!", "Boom!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Sweet!", "Sweet!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "I make this look good.", "I make this look good."}
    }
};

// Dismissiveness
static const ChatMenuList dismissiveness_menu{
    .display_string = "DISMISSIVE",
    .type = ChatMenuListType::Basic,
    .elements = {
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Sucks to be you!", "Sucks to be you!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "You are so dead.", "You are so dead."},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Owned!", "Owned!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Fresh meat!", "Fresh meat!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Lamer!", "Lamer!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Target practice!", "Target practice!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Catch!", "Catch!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Blams!", "Blams!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Sit down!", "Sit down!"}
    }
};

// Bravado
static const ChatMenuList bravado_menu{
    .display_string = "BRAVADO",
    .type = ChatMenuListType::Basic,
    .elements = {
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Get on the bus!", "Get on the bus!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Take off, hoser!", "Take off, hoser!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Want some more?!", "Want some more?!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Give it up!", "Give it up!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Bring it!", "Bring it!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Any time, anywhere!", "Any time, anywhere!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Tool!", "Tool!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Oh, I still love you!", "Oh, I still love you!"}
    }
};

// Derision
static const ChatMenuList derision_menu{
    .display_string = "DERISION",
    .type = ChatMenuListType::Basic,
    .elements = {
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Feeble!", "Feeble!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Chump!", "Chump!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Order up!", "Order up!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "What's your name, scumbag?!", "What's your name, scumbag?!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Lay down, play dead!", "Lay down, play dead!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Crunch time!", "Crunch time!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Splat!", "Splat!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Annihilation!", "Annihilation!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "You lack discipline!", "You lack discipline!"}
    }
};

// Casual
static const ChatMenuList casual_menu{
    .display_string = "CASUAL",
    .type = ChatMenuListType::Basic,
    .elements = {
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Here's Johnny!", "Here's Johnny!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Nice catch!", "Nice catch!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Hey, is this your head?", "Hey, is this your head?"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "What's up, fool?!", "What's up, fool?!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Aw yeah!", "Aw yeah!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Eat it!", "Eat it!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Catch!", "Catch!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Target practice!", "Target practice!"}
    }
};

// Random/Funny
static const ChatMenuList random_funny_menu{
    .display_string = "RANDOM",
    .type = ChatMenuListType::Basic,
    .elements = {
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Geeze, what smells?", "Geeze, what smells?"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Oh, I still love you!", "Oh, I still love you!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Rest in pieces.", "Rest in pieces."},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Want some more?!", "Want some more?!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Give it up!", "Give it up!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Squeegee time!", "Squeegee time!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Aw yeah!", "Aw yeah!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Arr matey!", "Arr matey!"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Get a load of this!", "Get a load of this!"}
    }
};

// Command
static const ChatMenuList command_menu{
    .display_string = "COMMANDS",
    .type = ChatMenuListType::Basic,
    .elements = {
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Vote next map", "/vote next"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Vote previous map", "/vote previous"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Vote restart map", "/vote restart"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Vote extend round", "/vote extend"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Print my stats", "/stats"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Which map is next?", "/nextmap"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Who isn't ready?", "/whosready"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Print match info", "/matchinfo"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Print server info", "/info"}
    }
};

// Spectate
static const ChatMenuList spectate_menu{
    .display_string = "SPECTATE MODE",
    .type = ChatMenuListType::Basic,
    .elements = {
        //{false, ChatMenuListName::Null, ChatMenuListType::Basic, "Free camera", "spectate"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Follow killer", "spectate_followkiller"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Minimal UI", "spectate_minui"},
        {false, ChatMenuListName::Null, ChatMenuListType::Basic, "Player labels", "spectate_playerlabels"},
    }
};

// Level (blank, built during level post init if applicable)
static ChatMenuList level_menu;

static ChatMenuType g_chat_menu_active = ChatMenuType::None;
static const ChatMenuList* g_active_menu = &radio_messages_menu;
static const ChatMenuList* g_previous_menu = nullptr;
static bool g_level_chat_menu_present = false;
rf::TimestampRealtime g_chat_menu_timer;
rf::TimestampRealtime g_taunt_timer;
rf::TimestampRealtime g_rad_msg_timer;

bool is_element_valid(const ChatMenuElement& element) {
    if (element.type == ChatMenuListType::Basic) {
        return true;
    }
    if (element.type == ChatMenuListType::CTF && rf::multi_get_game_type() == rf::NG_TYPE_CTF) {
        return true;
    }
    if (element.type == ChatMenuListType::TeamMode && rf::multi_get_game_type() != rf::NG_TYPE_DM) {
        return true;
    }
    if (element.type == ChatMenuListType::Map && g_level_chat_menu_present && !element.display_string.empty()) {
        return true;
    }
    return false;
}

bool get_chat_menu_is_active() {
    return g_chat_menu_active != ChatMenuType::None;
}

namespace rf
{
    auto& hud_miniflag_red_bmh = addr_as_ref<int>(0x0059DF48);
    auto& hud_miniflag_blue_bmh = addr_as_ref<int>(0x0059DF4C);
    auto& hud_miniflag_hilight_bmh = addr_as_ref<int>(0x0059DF50);
    auto& hud_flag_red_bmh = addr_as_ref<int>(0x0059DF54);
    auto& hud_flag_blue_bmh = addr_as_ref<int>(0x0059DF58);
    auto& hud_flag_gr_mode = addr_as_ref<rf::gr::Mode>(0x01775B30);
}

void hud_render_team_scores()
{
    int clip_h = rf::gr::clip_height();
    rf::gr::set_color(0, 0, 0, 150);
    int box_w = g_big_team_scores_hud ? 370 : 185;
    int box_h = g_big_team_scores_hud ? 80 : 55;
    int box_x = 10;
    int box_y = clip_h - box_h - 10; // clip_h - 65
    int miniflag_x = box_x + 7; // 17
    int miniflag_label_x = box_x + (g_big_team_scores_hud ? 45 : 33); // 43
    int max_miniflag_label_w = box_w - (g_big_team_scores_hud ? 80 : 55);
    int red_miniflag_y = box_y + 4; // clip_h - 61
    int blue_miniflag_y = box_y + (g_big_team_scores_hud ? 42 : 30); // clip_h - 35
    int red_miniflag_label_y = red_miniflag_y + 4; // clip_h - 57
    int blue_miniflag_label_y = blue_miniflag_y + 4; // clip_h - 31
    int flag_x = g_big_team_scores_hud ? 410 : 205;
    float flag_scale = g_big_team_scores_hud ? 1.5f : 1.0f;

    rf::gr::rect(10, clip_h - box_h - 10, box_w, box_h);
    auto game_type = rf::multi_get_game_type();
    int font_id = hud_get_default_font();

    if (game_type == rf::NG_TYPE_CTF) {
        static float hud_flag_alpha = 255.0f;
        static bool hud_flag_pulse_dir = false;
        float delta_alpha = rf::frametime * 500.0f;
        if (hud_flag_pulse_dir) {
            hud_flag_alpha -= delta_alpha;
            if (hud_flag_alpha <= 50.0f) {
                hud_flag_alpha = 50.0f;
                hud_flag_pulse_dir = false;
            }
        }
        else {
            hud_flag_alpha += delta_alpha;
            if (hud_flag_alpha >= 255.0f) {
                hud_flag_alpha = 255.0f;
                hud_flag_pulse_dir = true;
            }
        }
        rf::gr::set_color(53, 207, 22, 255);
        rf::Player* red_flag_player = g_debug_team_scores_hud ? rf::local_player : rf::multi_ctf_get_red_flag_player();
        if (red_flag_player) {
            const char* name = red_flag_player->name;
            std::string fitting_name = hud_fit_string(name, max_miniflag_label_w, nullptr, font_id);
            rf::gr::string(miniflag_label_x, red_miniflag_label_y, fitting_name.c_str(), font_id);

            if (red_flag_player == rf::local_player) {
                rf::gr::set_color(255, 255, 255, static_cast<int>(hud_flag_alpha));
                hud_scaled_bitmap(rf::hud_flag_red_bmh, flag_x, box_y, flag_scale, rf::hud_flag_gr_mode);
            }
        }
        else if (rf::multi_ctf_is_red_flag_in_base()) {
            rf::gr::string(miniflag_label_x, red_miniflag_label_y, "at base", font_id);
        }
        else {
            rf::gr::string(miniflag_label_x, red_miniflag_label_y, "missing", font_id);
        }
        rf::gr::set_color(53, 207, 22, 255);
        rf::Player* blue_flag_player = rf::multi_ctf_get_blue_flag_player();
        if (blue_flag_player) {
            const char* name = blue_flag_player->name;
            std::string fitting_name = hud_fit_string(name, max_miniflag_label_w, nullptr, font_id);
            rf::gr::string(miniflag_label_x, blue_miniflag_label_y, fitting_name.c_str(), font_id);

            if (blue_flag_player == rf::local_player) {
                rf::gr::set_color(255, 255, 255, static_cast<int>(hud_flag_alpha));
                hud_scaled_bitmap(rf::hud_flag_blue_bmh, flag_x, box_y, flag_scale, rf::hud_flag_gr_mode);
            }
        }
        else if (rf::multi_ctf_is_blue_flag_in_base()) {
            rf::gr::string(miniflag_label_x, blue_miniflag_label_y, "at base", font_id);
        }
        else {
            rf::gr::string(miniflag_label_x, blue_miniflag_label_y, "missing", font_id);
        }
    }

    if (game_type == rf::NG_TYPE_CTF || game_type == rf::NG_TYPE_TEAMDM) {
        float miniflag_scale = g_big_team_scores_hud ? 1.5f : 1.0f;
        rf::gr::set_color(255, 255, 255, 255);
        if (rf::local_player) {
            int miniflag_hilight_y;
            if (rf::local_player->team == rf::TEAM_RED) {
                miniflag_hilight_y = red_miniflag_y;
            }
            else {
                miniflag_hilight_y = blue_miniflag_y;
            }
            hud_scaled_bitmap(rf::hud_miniflag_hilight_bmh, miniflag_x, miniflag_hilight_y, miniflag_scale, rf::hud_flag_gr_mode);
        }
        hud_scaled_bitmap(rf::hud_miniflag_red_bmh, miniflag_x, red_miniflag_y, miniflag_scale, rf::hud_flag_gr_mode);
        hud_scaled_bitmap(rf::hud_miniflag_blue_bmh, miniflag_x, blue_miniflag_y, miniflag_scale, rf::hud_flag_gr_mode);
    }

    int red_score, blue_score;
    if (g_debug_team_scores_hud) {
        red_score = 15;
        blue_score = 15;
    }
    else if (game_type == rf::NG_TYPE_CTF) {
        red_score = rf::multi_ctf_get_red_team_score();
        blue_score = rf::multi_ctf_get_blue_team_score();
    }
    else if (game_type == rf::NG_TYPE_TEAMDM) {
        rf::gr::set_color(53, 207, 22, 255);
        red_score = rf::multi_tdm_get_red_team_score();
        blue_score = rf::multi_tdm_get_blue_team_score();
    }
    else {
        red_score = 0;
        blue_score = 0;
    }
    auto red_score_str = std::to_string(red_score);
    auto blue_score_str = std::to_string(blue_score);
    int str_w, str_h;
    rf::gr::get_string_size(&str_w, &str_h, red_score_str.c_str(), -1, font_id);
    rf::gr::string(box_x + box_w - 5 - str_w, red_miniflag_label_y, red_score_str.c_str(), font_id);
    rf::gr::get_string_size(&str_w, &str_h, blue_score_str.c_str(), -1, font_id);
    rf::gr::string(box_x + box_w - 5 - str_w, blue_miniflag_label_y, blue_score_str.c_str(), font_id);
}

CallHook<void(int, int, int, rf::gr::Mode)> hud_render_power_ups_gr_bitmap_hook{
    {
        0x0047FF2F,
        0x0047FF96,
        0x0047FFFD,
    },
    [](int bm_handle, int x, int y, rf::gr::Mode mode) {
        float scale = g_alpine_game_config.big_hud ? 2.0f : 1.0f;
        x = hud_transform_value(x, 640, rf::gr::clip_width());
        x = hud_scale_value(x, rf::gr::clip_width(), scale);
        y = hud_scale_value(y, rf::gr::clip_height(), scale);
        hud_scaled_bitmap(bm_handle, x, y, scale, mode);
    },
};

FunHook<void()> render_level_info_hook{
    0x00477180,
    []() {
        gr_font_run_with_default(hud_get_default_font(), [&]() {
            render_level_info_hook.call_target();
        });
    },
};

FunHook<void()> multi_hud_init_hook{
    0x00476AD0,
    []() {
        // Change font for Time Left text
        static int time_left_font = rf::gr::load_font("rfpc-large.vf");
        if (time_left_font >= 0) {
            write_mem<i8>(0x00477157 + 1, time_left_font);
        }
        multi_hud_init_hook.call_target();
    },
};

void hud_render_ready_notification()
{
    std::string ready_key_text =
        get_action_bind_name(get_af_control(rf::AlpineControlConfigAction::AF_ACTION_READY));

    std::string ready_notification_text =
        "Press " + ready_key_text + " to ready up for the match";

    rf::gr::set_color(255, 255, 255, 225);
    int center_x = rf::gr::screen_width() / 2;
    int notification_y = static_cast<int>(rf::gr::screen_height() * 0.25f);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, center_x, notification_y, ready_notification_text.c_str(), 0);
}

void draw_hud_ready_notification(bool draw)
{
    draw ? g_draw_ready_notification = true : g_draw_ready_notification = false;
}

void set_local_pre_match_active(bool set_active) {
    set_active ? g_pre_match_active = true : g_pre_match_active = false;

    draw_hud_ready_notification(set_active);
}

void hud_render_vote_notification()
{
    std::string vote_yes_key_text =
        get_action_bind_name(get_af_control(rf::AlpineControlConfigAction::AF_ACTION_VOTE_YES));

    std::string vote_no_key_text =
        get_action_bind_name(get_af_control(rf::AlpineControlConfigAction::AF_ACTION_VOTE_NO));

    std::string vote_notification_text =
        "ACTIVE QUESTION: \n" + g_active_vote_type + "\n\n" + vote_yes_key_text + " to vote yes\n" + vote_no_key_text + " to vote no";

    rf::gr::set_color(255, 255, 255, 225);
    int notification_y = static_cast<int>(rf::gr::screen_height() * 0.25f);
    rf::gr::string_aligned(rf::gr::ALIGN_LEFT, 8, notification_y, vote_notification_text.c_str(), 0);
}

void draw_hud_vote_notification(std::string vote_type)
{
    if (!vote_type.empty()) {
        g_draw_vote_notification = true;
        g_active_vote_type = vote_type;
    }
}

void remove_hud_vote_notification()
{
    g_draw_vote_notification = false;
    g_active_vote_type = "";
}

CodeInjection hud_render_patch_alpine {
    0x00476D9D,
    [](auto& regs) {
        if (g_draw_vote_notification) {
            hud_render_vote_notification();
        }

        if (g_draw_ready_notification) {
            hud_render_ready_notification();
        }

        if (g_chat_menu_active != ChatMenuType::None) {
            hud_render_draw_chat_menu();

            if (!g_chat_menu_timer.valid() || g_chat_menu_timer.elapsed()) {
                toggle_chat_menu(ChatMenuType::None);
            }
        }
    }
};

void build_chat_menu_comms_messages() {
    level_menu = ChatMenuList{
        .display_string = "MAP MESSAGES",
        .type = ChatMenuListType::Map,
        .elements = {}
    };

    static const std::array<AlpineLevelInfoID, 9> map_keys = {{
        AlpineLevelInfoID::ChatMap1,
        AlpineLevelInfoID::ChatMap2,
        AlpineLevelInfoID::ChatMap3,
        AlpineLevelInfoID::ChatMap4,
        AlpineLevelInfoID::ChatMap5,
        AlpineLevelInfoID::ChatMap6,
        AlpineLevelInfoID::ChatMap7,
        AlpineLevelInfoID::ChatMap8,
        AlpineLevelInfoID::ChatMap9
    }};

    bool has_valid_entries = false;

    for (const auto& id : map_keys) {
        if (g_alpine_level_info_config.is_option_loaded(rf::level.filename, id)) {
            std::string msg = get_level_info_value<std::string>(rf::level.filename, id);
            level_menu.elements.push_back(
                {false, ChatMenuListName::Null, ChatMenuListType::Map, msg, msg}
            );
            has_valid_entries = true;
        }
        else {
            // Insert a blank placeholder if not found
            level_menu.elements.push_back({false, ChatMenuListName::Null, ChatMenuListType::Map, "", ""});
        }
    }

    g_level_chat_menu_present = has_valid_entries;
}

void draw_chat_menu_text(int x, int y) {
    if (!g_active_menu) {
        return;
    }

    std::ostringstream main_string;
    // Get the menu name
    std::string menu_name = g_active_menu->display_string;

    main_string << menu_name << "\n\n";

    int display_index = 1; // Number to display
    constexpr size_t max_length = 19; // Max allowed characters before truncation

    for (const auto& element : g_active_menu->elements) {
        // If the element is not valid for this game mode, skip it completely (no blank slot)
        if (!is_element_valid(element)) {
            main_string << "\n";
            ++display_index; // Skip this number
            continue;
        }

        // Otherwise, display the menu item, truncating if too long
        std::string display_name = element.display_string.empty() ? element.long_string : element.display_string;
        if (display_name.length() > max_length) {
            display_name = display_name.substr(0, max_length) + "...";
        }
        main_string << display_index << ": " << display_name << "\n";

        ++display_index;
    }

    // Add back or exit option
    if (g_previous_menu) {
        main_string << "\n0: BACK\n";
    }
    else {
        main_string << "\n0: EXIT\n";
    }

    // Draw text
    int str_x = x + 4;
    int str_y = y + 4;
    rf::gr::set_color(255, 255, 180, 0xCC);
    rf::gr::string_aligned(rf::gr::ALIGN_LEFT, str_x, str_y, main_string.str().c_str(), 1);
}

void hud_render_draw_chat_menu() {
    int w = static_cast<int>(200);
    int h = static_cast<int>(166);
    int x = static_cast<int>(10);
    int y = (static_cast<int>(rf::gr::screen_height()) - h) / 2;
    rf::gr::set_color(0, 0, 0, 0x80);
    rf::gr::rect(x, y, w, h);
    rf::gr::set_color(79, 216, 255, 0x80);
    rf::gr::rect_border(x, y, w, h);

    draw_chat_menu_text(x, y);
}

void set_chat_menu_state(ChatMenuType state) {
    g_chat_menu_active = state;

    if (g_chat_menu_active == ChatMenuType::None) {
        g_chat_menu_timer.invalidate();
        g_active_menu = nullptr;
        g_previous_menu = nullptr;
    }
}

void chat_menu_go_back()
{
    if (g_previous_menu) {
        g_active_menu = g_previous_menu;

        // special handling for third depth level to avoid having to store a stack of previous menus
        // if expanded in the future to have more third depth levels, this will need to be handled differently
        if (g_active_menu == &general_menu) {
            g_previous_menu = &radio_messages_menu;
        }
        else {
            g_previous_menu = nullptr;
        }
    }
    else {
        set_chat_menu_state(ChatMenuType::None);
    }
}

void toggle_chat_menu(ChatMenuType type) {
    if (g_chat_menu_active == type) {
        set_chat_menu_state(ChatMenuType::None);
    } else {
        g_chat_menu_active = type;
        g_chat_menu_timer.set(5000); // 5 sec timeout
        g_previous_menu = nullptr;

        // Determine the new active menu
        switch (type) {
            case ChatMenuType::Comms:
                g_active_menu = (rf::multi_get_game_type() == rf::NG_TYPE_DM) ? &express_menu : &radio_messages_menu;
                break;
            case ChatMenuType::Taunts:
                g_active_menu = &taunt_menu;
                break;
            case ChatMenuType::Commands:
                g_active_menu = &command_menu;
                break;
            case ChatMenuType::Spectate:
                g_active_menu = &spectate_menu;
                break;
            default:
                set_chat_menu_state(ChatMenuType::None);
                break;
        }
    }
}

// call only when chat menu is active // disappearing base menu on 0 idk
void chat_menu_action_handler(rf::Key key) {
    g_chat_menu_timer.set(5000); // Menu timeout (5 sec)

    // Handle "Go Back" when pressing 0
    if (key == rf::KEY_0) {
        chat_menu_go_back();
        return;
    }

    int index = static_cast<int>(key) - static_cast<int>(rf::KEY_1);
    if (!g_active_menu) {
        return;
    }

    int menu_size = static_cast<int>(g_active_menu->elements.size());
    if (index < 0 || index >= menu_size) {
        return;
    }

    const ChatMenuElement& selected_element = g_active_menu->elements[index];

    if (!is_element_valid(selected_element)) {
        return; // Don't process elements that don't match the game mode
    }

    if (selected_element.is_menu) {
        // Store the current menu in history for backtracking
        g_previous_menu = g_active_menu;
        
        // Submenu transitions
        switch (selected_element.menu) {
            // Radio messages menu
            case ChatMenuListName::General: g_active_menu = &general_menu; break;
            case ChatMenuListName::Express: g_active_menu = &express_menu; break;
            case ChatMenuListName::Compliment: g_active_menu = &compliment_menu; break;
            case ChatMenuListName::Respond: g_active_menu = &respond_menu; break;
            case ChatMenuListName::AttackDefend: g_active_menu = &attack_defend_menu; break;
            case ChatMenuListName::Enemy: g_active_menu = &enemy_menu; break;
            case ChatMenuListName::Timing: g_active_menu = &timing_menu; break;
            case ChatMenuListName::Powerup: g_active_menu = &powerup_menu; break;
            case ChatMenuListName::Flag: g_active_menu = &ctf_menu; break;
            case ChatMenuListName::Map: g_active_menu = &level_menu; break;

            // Taunt menu
            case ChatMenuListName::Intimidation: g_active_menu = &intimidation_menu; break;
            case ChatMenuListName::Mockery: g_active_menu = &mockery_menu; break;
            case ChatMenuListName::Celebration: g_active_menu = &celebration_menu; break;
            case ChatMenuListName::Dismissiveness: g_active_menu = &dismissiveness_menu; break;
            case ChatMenuListName::Bravado: g_active_menu = &bravado_menu; break;
            case ChatMenuListName::Derision: g_active_menu = &derision_menu; break;
            case ChatMenuListName::Casual: g_active_menu = &casual_menu; break;
            case ChatMenuListName::RandomFunny: g_active_menu = &random_funny_menu; break;

            default: break;
        }
    } else {
        // Determine chat behavior based on menu type
        if (g_chat_menu_active == ChatMenuType::Taunts) {
            if (!g_taunt_timer.valid() || g_taunt_timer.elapsed()) {
                g_taunt_timer.set(10000); // 10 second cooldown
                const std::string msg = "\xA8[Taunt] " + selected_element.long_string;
                rf::multi_chat_say(("\xA8[Taunt] " + selected_element.long_string).c_str(), false);
                rf::snd_play(4, 0, 0.0f, 1.0f);
            } else {
                rf::String msg{"You must wait a little while between taunts"};
                rf::multi_chat_print(msg, rf::ChatMsgColor::white_white, {});
            }
        }
        else if (g_chat_menu_active == ChatMenuType::Commands) {
            // Commands do not play a chat sound or display for user
            const std::string msg = selected_element.long_string;
            if (!msg.empty()) {
                send_chat_line_packet(msg.c_str(), nullptr);
            }
        }
        else if (g_chat_menu_active == ChatMenuType::Spectate) {
            // Spectate menu commands go directly to console
            const std::string console_cmd = selected_element.long_string;
            if (!console_cmd.empty()) {
                rf::console::do_command(console_cmd.c_str());
            }
        } 
        else {
            // Default chat behavior
            volatile bool use_team_chat = (g_active_menu->type != ChatMenuListType::Basic);
            const std::string msg = "\xA8 " + selected_element.long_string;
            if (!msg.empty()) {
                if (!g_rad_msg_timer.valid() || g_rad_msg_timer.elapsed()) {
                    g_rad_msg_timer.set(1000);
                    rf::multi_chat_say(msg.c_str(), use_team_chat);
                    rf::snd_play(4, 0, 0.0f, 1.0f);
                }
                else {
                    rf::String msg{"You must wait at least one second between radio messages"};
                    rf::multi_chat_print(msg, rf::ChatMsgColor::white_white, {});
                }
            }
        }

        // Close menu after sending message
        toggle_chat_menu(ChatMenuType::None);
    }
}

FunHook<void(rf::Player*, const char*, int)> chat_add_msg_hook{
    0x00443FB0,
    [](rf::Player* player, const char* message, int message_type) {

        handle_chat_message_sound(message);

        chat_add_msg_hook.call_target(player, message, message_type);
    },
};

// displays name of player you're pointing at
CallHook<void(int, int, const char*, int, rf::gr::Mode)> display_target_player_name_hook{
    0x00478140,
    [](int x, int y, const char* s, int font_num, rf::gr::Mode mode) {
        if (!g_alpine_game_config.display_target_player_names) {
            return;
        }
        display_target_player_name_hook.call_target(x, y, s, font_num, mode);
    },
};

ConsoleCommand2 playernames_cmd{
    "mp_playernames",
    []() {
        g_alpine_game_config.display_target_player_names = !g_alpine_game_config.display_target_player_names;
        rf::console::print("Display of names of targeted players is {}", g_alpine_game_config.display_target_player_names ? "enabled" : "disabled");
    },
    "Toggle displaying names of targeted players",
    "mp_playernames",
};

FunHook<void()> multi_hud_render_time_left_hook{
    0x004770A0,
    []() {
        if (!rf::time_left_visible || rf::time_left_seconds < 0 || rf::time_left_minutes < 0 || rf::time_left_hours < 0) {
        return;
    }

    if (rf::time_left_fade_in == 1) {
        rf::time_left_alpha += rf::frametime * 500.0f;
        if (rf::time_left_alpha > 192.0) {
            rf::time_left_alpha = 192.0;
            rf::time_left_fade_in = 0;
        }
    }

    std::string time_left_string = std::format("{}{:02}:{:02}:{:02}", time_left_string_format,
        rf::time_left_hours, rf::time_left_minutes, rf::time_left_seconds);

    // set timer color, including alpha adjustment for fade in
    rf::gr::set_color(
        std::get<0>(time_left_string_color),
        std::get<1>(time_left_string_color),
        std::get<2>(time_left_string_color),
        static_cast<int>(std::get<3>(time_left_string_color) * (rf::time_left_alpha / 255.0f)));

    // Use custom offset if specified, otherwise use default positioning
    int x_pos, y_pos;
    if (g_alpine_game_config.timer_hud_offset.x != -1) {
        x_pos = g_alpine_game_config.timer_hud_offset.x;
    } else {
        x_pos = rf::gr::clip_width() - time_left_string_x_pos_offset;
    }
    
    if (g_alpine_game_config.timer_hud_offset.y != -1) {
        y_pos = g_alpine_game_config.timer_hud_offset.y;
    } else {
        y_pos = rf::gr::clip_height() - time_left_string_y_pos_offset;
    }

    rf::gr::string_aligned(rf::gr::ALIGN_LEFT, x_pos, y_pos, time_left_string.c_str(), hud_get_timer_font(), rf::gr::text_2d_mode);
    },
};

void build_time_left_string_format() {
    
    if (g_alpine_game_config.verbose_time_left_display) {
        auto language = rf::get_language();
        switch (language) {
            case 0: time_left_string_format = "Time Left: "; break;
            case 1: time_left_string_format = "Verbleibende Zeit: "; break;
            case 2: time_left_string_format = "Temps restant: "; break;
            default: time_left_string_format = "";
        }
    } else {
        time_left_string_format = "";
    }

    int format_text_width = 0, format_text_height = 0;

    rf::gr::gr_get_string_size(&format_text_width, &format_text_height, 
                               time_left_string_format.c_str(),
                               time_left_string_format.size(), hud_get_timer_font());

    time_left_string_x_pos_offset = 135 + format_text_width;
    time_left_string_y_pos_offset = 21;

    if (g_alpine_options_config.is_option_loaded(AlpineOptionID::MultiTimerXOffset)) {
        int x_offset = std::get<int>(g_alpine_options_config.options[AlpineOptionID::MultiTimerXOffset]);
        xlog::warn("applying offset X {}", x_offset);
        time_left_string_x_pos_offset -= x_offset;
    }

    if (g_alpine_options_config.is_option_loaded(AlpineOptionID::MultiTimerYOffset)) {
        int y_offset = std::get<int>(g_alpine_options_config.options[AlpineOptionID::MultiTimerYOffset]);
        xlog::warn("applying offset Y {}", y_offset);
        time_left_string_y_pos_offset -= y_offset;
    }

    if (g_alpine_options_config.is_option_loaded(AlpineOptionID::MultiTimerColor)) {
        auto timer_color = std::get<uint32_t>(g_alpine_options_config.options[AlpineOptionID::MultiTimerColor]);
        int red, green, blue, alpha;
        std::tie(red, green, blue, alpha) = extract_color_components(timer_color);
        time_left_string_color = {red, green, blue, alpha};
    }
}

ConsoleCommand2 verbosetimer_cmd{
    "mp_verbosetimer",
    []() {
        g_alpine_game_config.verbose_time_left_display = !g_alpine_game_config.verbose_time_left_display;
        build_time_left_string_format();
        rf::console::print("Verbose in-game timer display is {}", g_alpine_game_config.verbose_time_left_display ? "enabled" : "disabled");
    },
    "Control whether the in-game timer displays the 'Time Left:' text",
    "mp_verbosetimer",
};

void multi_hud_apply_patches()
{
    hud_render_patch_alpine.install();
    AsmWriter{0x00477790}.jmp(hud_render_team_scores);
    hud_render_power_ups_gr_bitmap_hook.install();
    render_level_info_hook.install();
    multi_hud_init_hook.install();

    // Drawing of targeted player names in multi
    display_target_player_name_hook.install();

    // Play radio message and taunt sounds
    chat_add_msg_hook.install();

    // Draw Time Left label
    multi_hud_render_time_left_hook.install();

    // Console commands
    playernames_cmd.register_cmd();
    verbosetimer_cmd.register_cmd();
}

void multi_hud_set_big(bool is_big)
{
    g_big_team_scores_hud = is_big;
}
