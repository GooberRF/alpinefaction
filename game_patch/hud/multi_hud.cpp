#include <ranges>
#include <algorithm>
#include <tuple>
#include <format>
#include <cstdint>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <common/utils/list-utils.h>
#include <common/utils/string-utils.h>
#include <xlog/xlog.h>
#include "../multi/multi.h"
#include "../multi/gametype.h"
#include "../input/input.h"
#include "../rf/input.h"
#include "../rf/hud.h"
#include "../rf/level.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../rf/os/frametime.h"
#include "../rf/os/timer.h"
#include "../rf/os/timestamp.h"
#include "../rf/os/console.h"
#include "../rf/os/os.h"
#include "../rf/gameseq.h"
#include "../misc/player.h"
#include "../main/main.h"
#include "../graphics/gr.h"
#include "../misc/alpine_options.h"
#include "../misc/alpine_settings.h"
#include "../sound/sound.h"
#include "../os/console.h"
#include "hud_internal.h"
#include "hud.h"
#include "multi_scoreboard.h"
#include "remote_server_cfg_ui.h"
#include "../misc/player.h"
#include "../multi/alpine_packets.h"
#include "../multi/network.h"

static bool g_big_team_scores_hud = false;
constexpr bool g_debug_team_scores_hud = false;
static bool g_draw_vote_notification = false;
static std::string g_active_vote_type = "";
static bool g_draw_ready_notification = false;
bool g_pre_match_active = false;
static bool g_draw_respawn_timer_notification = false;
static bool g_draw_respawn_timer_can_respawn = false;
static std::string time_left_string_format = "";
static int time_left_string_x_pos_offset = 135;
static int time_left_string_y_pos_offset = 21;
static rf::Color time_left_string_color = {0, 255, 0, 255};
static rf::TimestampRealtime g_run_life_start_timestamp;
static bool g_run_timer_reset_by_respawn_key = false;
static bool g_run_timer_fade_active = false;

void multi_hud_update_timer_color()
{
    // default
    time_left_string_color = {0, 255, 0, 255};

    if (g_alpine_options_config.is_option_loaded(AlpineOptionID::MultiTimerColor)) {
        auto timer_color = get_option_value<uint32_t>(AlpineOptionID::MultiTimerColor);
        time_left_string_color = rf::Color::from_hex(timer_color);
    }
    else if (g_alpine_game_config.multi_timer_color_override) {
        time_left_string_color = rf::Color::from_hex(*g_alpine_game_config.multi_timer_color_override);
    }
}

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
    if (element.type == ChatMenuListType::TeamMode && multi_is_team_game_type()) {
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

static inline void cp_owner_color(HillOwner owner, rf::ubyte& r, rf::ubyte& g, rf::ubyte& b, rf::ubyte& a)
{
    a = 220;
    switch (owner) {
        case HillOwner::HO_Red:
            r = 167; g = 0;b = 0;
            return;
        case HillOwner::HO_Blue:
            r = 52; g = 78; b = 167;
            return;
        default: // neutral
            r = 0; g = 0; b = 0;
            return; 
    }
}

static inline void cp_steal_color(HillOwner steal_dir, rf::ubyte& r, rf::ubyte& g, rf::ubyte& b, rf::ubyte& a)
{
    a = 230;
    if (steal_dir == HillOwner::HO_Red) {
        r = 200; g = 40; b = 40;
        return;
    }
    if (steal_dir == HillOwner::HO_Blue) {
        r = 70; g = 100; b = 200;
        return;
    }
    r = 180; g = 180; b = 180;
}

static void hud_draw_cp_row_fullwidth(int x, int y, int w, int h, const HillInfo& hinfo, int font_id)
{
    // background base colour
    rf::gr::set_color(0, 0, 0, 150);
    rf::gr::rect(x, y, w, h);

    // fill with owner colour
    rf::ubyte or_, og, ob, oa;
    cp_owner_color(hinfo.ownership, or_, og, ob, oa);
    rf::gr::set_color(or_, og, ob, 130);
    rf::gr::rect(x + 1, y + 1, w - 2, h - 2);

    // cap progress bar
    if (hinfo.steal_dir != HillOwner::HO_Neutral) {
        rf::ubyte pr, pg, pb, pa;
        cp_steal_color(hinfo.steal_dir, pr, pg, pb, pa);

        const float t = std::clamp(hinfo.capture_progress, (uint8_t)0, (uint8_t)100) / 100.0f;
        const int inner = std::max(1, int((w - 2) * t));

        rf::gr::set_color(pr, pg, pb, pa);
        rf::gr::rect(x + 1, y + 1, inner, h - 2);
    }

    // border
    rf::gr::set_color(150, 150, 150, 170);
    rf::gr::rect_border(x, y, w, h);

    // text
    const int pad_l = 6;
    const int pad_r = 6;
    const bool contested = (hinfo.steal_dir != HillOwner::HO_Neutral);
    const bool locked = (hinfo.lock_status != HillLockStatus::HLS_Available);
    const int pct_w_room = (contested || locked) ? 40 : 0; // show progress if contested or locked
    std::string name_to_show = hinfo.name.empty() ? "Point" : hinfo.name;
    std::string fit_name = hud_fit_string(name_to_show.c_str(), (w - pad_l - pad_r - pct_w_room), nullptr, font_id);

    // name
    const auto [tw, th] = rf::gr::get_string_size(fit_name, font_id);
    rf::gr::set_color(0, 0, 0, 220);
    rf::gr::string(x + pad_l + 1, y + (h - th) / 2 + 1, fit_name.c_str(), font_id); // shadow
    rf::gr::set_color(255, 255, 255, 255);
    rf::gr::string(x + pad_l, y + (h - th) / 2, fit_name.c_str(), font_id); // main

    // padlock or progress percentage
    if (locked) {
        static constexpr char kPadlockGlyph[] = {static_cast<char>(0xA7), '\0'}; // padlock
        static constexpr char kPermalockGlyph[] = {static_cast<char>(0xAB), '\0'}; // filled checkbox
        const char* lock_glyph =
            (hinfo.lock_status == HillLockStatus::HLS_Permalocked) ? kPermalockGlyph : kPadlockGlyph;
        const auto [lw, lh] = rf::gr::get_string_size(lock_glyph, font_id);
        rf::gr::set_color(0, 0, 0, 220);
        rf::gr::string(x + w - pad_r - lw + 1, y + (h - lh) / 2 + 1, lock_glyph, font_id); // shadow
        rf::gr::set_color(255, 255, 255, 255);
        rf::gr::string(x + w - pad_r - lw, y + (h - lh) / 2, lock_glyph, font_id); // main
    }
    else if (contested) {
        char pct_buf[5];
        std::snprintf(pct_buf, sizeof(pct_buf), "%d%%", (int)std::clamp(hinfo.capture_progress, (uint8_t)0, (uint8_t)100));
        const auto [pw, ph] = rf::gr::get_string_size(pct_buf, font_id);
        rf::gr::set_color(0, 0, 0, 220);
        rf::gr::string(x + w - pad_r - pw + 1, y + (h - ph) / 2 + 1, pct_buf, font_id); // shadow
        rf::gr::set_color(255, 255, 255, 255);
        rf::gr::string(x + w - pad_r - pw, y + (h - ph) / 2, pct_buf, font_id); // main
    }
}

static void hud_render_cp_strip_koth_dc_fullwidth(int anchor_x, int anchor_y, int anchor_w)
{
    if (!multi_is_game_type_with_hills())
        return;

    const int count = (int)g_koth_info.hills.size();
    if (count <= 0)
        return;

    const int font_id = hud_get_default_font();

    // size cp rows
    const bool big_ui = g_big_team_scores_hud;
    int row_h = big_ui ? 28 : 22;
    int gap_y = 4;
    int margin = 6; // margin above team scores box
    const int clip_h = rf::gr::clip_height();
    int total_h = count * row_h + (count - 1) * gap_y;
    int y0 = anchor_y - total_h - margin;

    if (y0 < 4) {
        // compress rows proportionally to keep gap ratio
        const int avail_h = std::max(8, anchor_y - margin - 4);
        if (avail_h < total_h) {
            const float s = std::max(14.0f, float(avail_h - (count - 1) * gap_y)) / float(count);
            row_h = int(std::floor(s));
            total_h = count * row_h + (count - 1) * gap_y;
            y0 = anchor_y - total_h - margin;
        }
    }

    // draw from top row to bottom, stacked
    int cur_y = y0;
    for (int i = 0; i < count; ++i) {
        const HillInfo& H = g_koth_info.hills[i];
        hud_draw_cp_row_fullwidth(anchor_x, cur_y, anchor_w, row_h, H, font_id);
        cur_y += row_h + gap_y;
    }
}

static void hud_render_koth_dc_split_scores(
    int x, int y, int w, int h, int red_score, int blue_score, int font, int bm_red, int bm_blue,
    int bm_hilight, rf::gr::Mode bm_mode, bool hilight_red, bool hilight_blue, float miniflag_scale
) {
    // base frame
    rf::gr::set_color(0, 0, 0, 150);
    rf::gr::rect(x, y, w, h);

    // halves
    const int half_w = w / 2;
    const int left_x = x;
    const int right_x = x + half_w;
    rf::gr::set_color(0, 0, 0, 160); // red
    rf::gr::rect(left_x + 1, y + 1, half_w - 2, h - 2);
    rf::gr::set_color(0, 0, 0, 160); // blue
    rf::gr::rect(right_x + 1, y + 1, w - half_w - 2, h - 2);

    // vertical separator
    rf::gr::set_color(255, 255, 255, 120);
    rf::gr::rect(x + half_w - 1, y + 2, 2, h - 4);

    // padding
    const int mid_y = y + h / 2;
    const int pad_outer = g_big_team_scores_hud ? 12 : 8; // dist from edge to flag
    const int pad_sep = g_big_team_scores_hud ? 10 : 8; // dist from separator to score text

    // miniflags pos
    const int kMiniW = int(16 * miniflag_scale);
    const int kMiniH = int(12 * miniflag_scale);

    // scores
    std::string rs = std::to_string(red_score);
    std::string bs = std::to_string(blue_score);
    const auto [rtw, rth] = rf::gr::get_string_size(rs, font);
    const auto [btw, bth] = rf::gr::get_string_size(bs, font);
    const int red_flag_x = left_x + pad_outer;
    const int red_flag_y = mid_y - (kMiniH);
    const int blue_flag_x = x + w - pad_outer - kMiniW;
    const int blue_flag_y = mid_y - (kMiniH);

    // red: right aligned to left side of separator
    const int red_tx = (x + half_w - pad_sep) - rtw;
    const int red_ty = mid_y - rth / 2;

    // blue: left aligned to right side of separator
    const int blue_tx = (x + half_w + pad_sep);
    const int blue_ty = mid_y - bth / 2;

    // miniflag highlight
    if (hilight_red)
        hud_scaled_bitmap(bm_hilight, red_flag_x, red_flag_y, miniflag_scale, bm_mode);
    if (hilight_blue)
        hud_scaled_bitmap(bm_hilight, blue_flag_x, blue_flag_y, miniflag_scale, bm_mode);

    // render miniflags
    hud_scaled_bitmap(bm_red, red_flag_x, red_flag_y, miniflag_scale, bm_mode);
    hud_scaled_bitmap(bm_blue, blue_flag_x, blue_flag_y, miniflag_scale, bm_mode);

    // render score text with shadow
    auto draw_text = [&](int tx, int ty, const std::string& s) {
        rf::gr::set_color(0, 0, 0, 230);
        rf::gr::string(tx + 1, ty + 1, s.c_str(), font);
        rf::gr::set_color(255, 255, 255, 255);
        rf::gr::string(tx, ty, s.c_str(), font);
    };
    draw_text(red_tx, red_ty, rs);
    draw_text(blue_tx, blue_ty, bs);

    // main border
    rf::gr::set_color(255, 255, 255, 170);
    rf::gr::rect_border(x, y, w, h);
}

static uint32_t run_timer_elapsed_milliseconds()
{
    if (!g_run_life_start_timestamp.valid() || !rf::local_player) {
        return 0;
    }

    return static_cast<uint32_t>(g_run_life_start_timestamp.time_since());
}

static std::string build_run_timer_string()
{
    if (!g_run_life_start_timestamp.valid()) {
        return "00:00:00.000";
    }

    const uint32_t elapsed_ms = run_timer_elapsed_milliseconds();
    const uint32_t total_seconds = elapsed_ms / 1000;

    const int hours = static_cast<int>(total_seconds / 3600);
    const int minutes = static_cast<int>((total_seconds % 3600) / 60);
    const int seconds = static_cast<int>(total_seconds % 60);
    const int milliseconds = static_cast<int>(elapsed_ms % 1000);

    return std::format("{:02}:{:02}:{:02}.{:03}", hours, minutes, seconds, milliseconds);
}

static std::tuple<int, int, int, int> get_run_timer_color()
{
    if (g_run_timer_reset_by_respawn_key && !g_run_life_start_timestamp.valid()) {
        return {255, 0, 0, 255};
    }

    if (g_run_timer_fade_active && g_run_life_start_timestamp.valid()) {
        const float elapsed_seconds = static_cast<float>(run_timer_elapsed_milliseconds()) / 1000.0f;
        const float t = std::clamp(elapsed_seconds / 10.0f, 0.0f, 1.0f);

        const int r = 255;
        const int g = static_cast<int>(255.0f * t);
        const int b = g;

        if (t >= 1.0f) {
            g_run_timer_fade_active = false;
        }

        return {r, g, b, 255};
    }

    return {255, 255, 255, 255};
}

static std::string build_run_timer_reset_label()
{
    if (!rf::local_player) {
        return "Reset: ?";
    }

    const rf::String key = get_action_bind_name(get_af_control(rf::AlpineControlConfigAction::AF_ACTION_SELF_KILL));
    return std::format("Reset: {}", key.c_str());
}

static void hud_render_run_timer_widget(int x, int y, int w, int h, int font_id)
{
    rf::gr::set_color(0, 0, 0, 150);
    rf::gr::rect(x, y, w, h);

    const int half_w = w / 2;
    rf::gr::set_color(0, 0, 0, 160);
    rf::gr::rect(x + 1, y + 1, half_w - 1, h - 2);
    rf::gr::rect(x + half_w, y + 1, w - half_w - 1, h - 2);

    rf::gr::set_color(255, 255, 255, 170);
    rf::gr::rect_border(x, y, w, h);

    auto draw_shadow_text = [&](int tx, int ty, const std::string& text, int font, int r, int g, int b, int a) {
        rf::gr::set_color(0, 0, 0, 230);
        rf::gr::string(tx + 1, ty + 1, text.c_str(), font);
        rf::gr::set_color(r, g, b, a);
        rf::gr::string(tx, ty, text.c_str(), font);
    };

    const std::string reset_label = build_run_timer_reset_label();
    const int label_font_id = hud_get_small_font();
    const auto [label_w, label_h] = rf::gr::get_string_size(reset_label, label_font_id);

    const std::string timer_string = build_run_timer_string();
    const auto [timer_w, timer_h] = rf::gr::get_string_size(timer_string, font_id);
    const int timer_x = x + (w - timer_w) / 2;
    const int center_x = x + w / 2;
    const int label_x = center_x - (label_w / 2);
    const int vertical_spacing = 2;
    const int content_h = label_h + vertical_spacing + timer_h;
    const int start_y = y + (h - content_h) / 2;
    const int label_y = start_y;
    const int timer_y = label_y + label_h + vertical_spacing;
    draw_shadow_text(label_x, label_y, reset_label, label_font_id, 180, 180, 180, 255);
    const auto [timer_r, timer_g, timer_b, timer_a] = get_run_timer_color();
    draw_shadow_text(timer_x, timer_y, timer_string, font_id, timer_r, timer_g, timer_b, timer_a);
}

void multi_hud_render_team_scores()
{
    int clip_h = rf::gr::clip_height();
    rf::gr::set_color(0, 0, 0, 150);

    const auto game_type = rf::multi_get_game_type();
    const bool is_koth_dc = (game_type == rf::NG_TYPE_KOTH || game_type == rf::NG_TYPE_DC);
    const bool is_esc = (game_type == rf::NG_TYPE_ESC);
    const bool is_rev = (game_type == rf::NG_TYPE_REV);
    const bool is_run = (game_type == rf::NG_TYPE_RUN);
    const bool is_hill_score = is_koth_dc || is_rev || is_esc;
    const bool show_run_timer = g_alpine_game_config.show_run_timer;

    if (is_run && !show_run_timer) {
        return;
    }

    int box_w = 0, box_h = 0;
    if (is_koth_dc || is_run) {
        box_w = g_big_team_scores_hud ? 240 : 185;
        box_h = g_big_team_scores_hud ? 60  : 40;
    } else if (is_rev || is_esc) {
        box_w = g_big_team_scores_hud ? 240 : 185;
    } else {
        const int ctf_box_w = rf::gr::clip_width() <= 1280 ? 350 : 370;
        box_w = g_big_team_scores_hud ? ctf_box_w : 185;
        box_h = g_big_team_scores_hud ? 80  : 55;
    }

    int box_x = 10;
    int box_y = clip_h - box_h - 10;
    int miniflag_x = box_x + 7;
    int miniflag_label_x = box_x + (g_big_team_scores_hud ? (is_koth_dc ? 40 : 45) : (is_koth_dc ? 30 : 33));
    int max_miniflag_label_w = box_w - (g_big_team_scores_hud ? (is_koth_dc ? 70 : 80) : (is_koth_dc ? 50 : 55));
    int red_miniflag_y = box_y + 4;
    int blue_miniflag_y = box_y + (g_big_team_scores_hud ? (is_koth_dc ? 38 : 42) : (is_koth_dc ? 28 : 30));
    int red_miniflag_label_y  = red_miniflag_y + 4;
    int blue_miniflag_label_y = blue_miniflag_y + 4;
    int flag_x = box_x + box_w + (g_big_team_scores_hud ? 30 : 10);
    float flag_scale = g_big_team_scores_hud ? 1.5f : 1.0f;

    if (!is_hill_score && !is_run) {
        rf::gr::rect(box_x, box_y, box_w, box_h);
    }
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

    if (multi_is_team_game_type() && !is_hill_score && !is_run) {
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

    int red_score = 0;
    int blue_score = 0;
    if (!is_run) {
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
        else if (game_type == rf::NG_TYPE_KOTH || game_type == rf::NG_TYPE_DC) {
            red_score = multi_koth_get_red_team_score();
            blue_score = multi_koth_get_blue_team_score();
        }
    }

    auto red_score_str = std::to_string(red_score);
    auto blue_score_str = std::to_string(blue_score);
    if (is_koth_dc) {
        hud_render_koth_dc_split_scores(
            box_x, box_y, box_w, box_h, red_score, blue_score, font_id, rf::hud_miniflag_red_bmh,
            rf::hud_miniflag_blue_bmh, rf::hud_miniflag_hilight_bmh, rf::hud_flag_gr_mode,
            (rf::local_player && rf::local_player->team == rf::TEAM_RED),
            (rf::local_player && rf::local_player->team == rf::TEAM_BLUE), (g_big_team_scores_hud ? 1.5f : 1.0f));
    }
    else if (is_run) {
        hud_render_run_timer_widget(box_x, box_y, box_w, box_h, font_id);
    }
    else if (!is_rev && !is_esc) {
        auto [str_w, str_h] = rf::gr::get_string_size(red_score_str, font_id);
        rf::gr::string(box_x + box_w - 5 - str_w, red_miniflag_label_y, red_score_str.c_str(), font_id);
        std::tie(str_w, str_h) = rf::gr::get_string_size(blue_score_str, font_id);
        rf::gr::string(box_x + box_w - 5 - str_w, blue_miniflag_label_y, blue_score_str.c_str(), font_id);
    }

    // render capture point bars
    if (is_koth_dc || is_rev || is_esc) {
        hud_render_cp_strip_koth_dc_fullwidth(box_x, box_y, box_w);
    }
}

CodeInjection multi_hud_render_team_scores_new_gamemodes_patch {
    0x00476DEB,
    [](auto& regs) {
        if (gt_is_koth() || gt_is_dc() || gt_is_rev() || gt_is_run() || gt_is_esc()) {
            regs.eip = 0x00476E06; // multi_hud_render_team_scores
        }
    }
};

CallHook<void(int, int, int, rf::gr::Mode)> multi_powerup_render_gr_bitmap_hook{
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

FunHook<void()> multi_hud_render_level_info_hook{
    0x00477180,
    []() {
        gr_font_run_with_default(hud_get_default_font(), [&]() {
            multi_hud_render_level_info_hook.call_target();
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

void hud_render_respawn_timer_notification() {
    const std::string notif_string = build_local_spawn_string(g_draw_respawn_timer_can_respawn);
    rf::gr::set_color(255, 255, 255, 225);
    const int center_x = rf::gr::screen_width() / 2;
    const int font = hud_get_default_font();
    const int notification_y = static_cast<int>(rf::gr::screen_height() * .925f);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, center_x, notification_y, notif_string.c_str(), font);
}

void stop_draw_respawn_timer_notification()
{
    g_draw_respawn_timer_notification = false;
    g_draw_respawn_timer_can_respawn = false;
}

void draw_respawn_timer_notification(bool can_respawn, bool force_respawn, int spawn_delay)
{
    g_draw_respawn_timer_notification = true;
    g_draw_respawn_timer_can_respawn = can_respawn;
}

void hud_render_ready_notification() {
    const std::string ready_key_text =
        get_action_bind_name(get_af_control(rf::AlpineControlConfigAction::AF_ACTION_READY));

    const std::string ready_notification_text =
        "Press " + ready_key_text + " to ready up for the match";

    rf::gr::set_color(255, 255, 255, 225);
    const int center_x = rf::gr::screen_width() / 2;
    const int font = hud_get_default_font();
    const int font_h = rf::gr::get_font_height(font);
    const int border = g_alpine_game_config.big_hud ? 3 : 2;
    const int hist_box_y = 10;
    const int hist_box_h = 8 * font_h + 2 * border + 6;
    const int input_box_y = hist_box_y + hist_box_h;
    const int input_box_content_h = font_h + 3;
    const int input_box_h = input_box_content_h + 2 * border;
    int notification_y = input_box_y + input_box_h;
    if (!g_alpine_game_config.big_hud) {
        notification_y += 2;
    }
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, center_x, notification_y, ready_notification_text.c_str(), font);
}

void draw_hud_ready_notification(bool draw)
{
    g_draw_ready_notification = draw;
}

void set_local_pre_match_active(bool set_active) {
    set_active ? g_pre_match_active = true : g_pre_match_active = false;

    draw_hud_ready_notification(set_active);
}

void hud_render_vote_notification() {
    const std::string vote_yes_key_text =
        get_action_bind_name(get_af_control(rf::AlpineControlConfigAction::AF_ACTION_VOTE_YES));

    const std::string vote_no_key_text =
        get_action_bind_name(get_af_control(rf::AlpineControlConfigAction::AF_ACTION_VOTE_NO));

    const std::string vote_notification_text =
        "ACTIVE QUESTION: \n" + g_active_vote_type + "\n\nPress " + vote_yes_key_text + " to vote yes\nPress " + vote_no_key_text + " to vote no";

    rf::gr::set_color(255, 255, 255, 225);
    const int font = hud_get_default_font();
    const int font_h = rf::gr::get_font_height(font);
    const int border = g_alpine_game_config.big_hud ? 3 : 2;
    const int hist_box_y = 10;
    const int hist_box_h = 8 * font_h + 2 * border + 6;
    int notification_y = hist_box_y + hist_box_h;
    if (!g_alpine_game_config.big_hud) {
        notification_y += 9;
    }
    rf::gr::string_aligned(rf::gr::ALIGN_LEFT, 10, notification_y, vote_notification_text.c_str(), font);
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

void build_local_player_spectators_strings() {
    g_local_player_spectators_spawned_string.clear();
    g_local_player_spectators_unspawned_string.clear();

    int num_players = 0;
    std::string names{};
    for (const rf::Player* const player : g_local_player_spectators) {
        if (num_players++) {
            if (num_players == g_local_player_spectators.size()) {
                if (g_local_player_spectators.size() == 2) {
                    names += " and ";
                } else {
                    names += ", and ";
                }
            } else {
                names += ", ";
            }
        }
        names += player->name.c_str();
    }

    g_local_player_spectators_unspawned_string = num_players == 1
        ? names + " is waiting to spectate you"
        : names + " are waiting to spectate you";
    g_local_player_spectators_spawned_string = num_players == 1
        ? names + " is spectating you"
        : names + " are spectating you";
}

void multi_hud_render_local_player_spectators() {
#if DBG_LOCAL_PLAYER_SPECTATORS
    g_local_player_spectators.clear();
    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        g_local_player_spectators.emplace(&player);
    }
    build_local_player_spectators_strings();
#endif
    const bool show_spectators = g_alpine_game_config.always_show_spectators
        || multi_scoreboard_is_visible();
    if (show_spectators
        && !g_local_player_spectators.empty()
        && !rf::hud_disabled
        && rf::gameseq_get_state() == rf::GS_GAMEPLAY) {
        const rf::NetGameType game_type = rf::multi_get_game_type();
        const bool is_koth_or_dc = game_type == rf::NG_TYPE_KOTH
            || game_type == rf::NG_TYPE_DC;
        const bool is_rev = game_type == rf::NG_TYPE_REV;
        const bool is_esc = game_type == rf::NG_TYPE_ESC;
        const int ctf_box_w = rf::gr::clip_width() <= 1280 ? 350 : 370;
        const int box_w = is_koth_or_dc || is_rev || is_esc
            ? g_alpine_game_config.big_hud ? 240 : 185
            : g_alpine_game_config.big_hud ? ctf_box_w : 185;
        constexpr int box_x = 10;

        int x = 10;
        if (multi_is_team_game_type()) {
            x += box_x + box_w;
            if (rf::multi_get_game_type() == rf::NG_TYPE_CTF) {
                const bool has_flag = rf::multi_ctf_get_blue_flag_player() == rf::local_player
                    || rf::multi_ctf_get_red_flag_player() == rf::local_player;
                if (has_flag) {
                    constexpr int flag_and_space_w = 111;
                    x += flag_and_space_w;
                }
            }
        }
        const int y = rf::gr::clip_height() - (g_alpine_game_config.big_hud ? 30 : 20);

        rf::gr::set_color(0, 255, 0, 255);
        const std::string& text = rf::player_is_dead(rf::local_player)
            || rf::player_is_dying(rf::local_player)
            ? g_local_player_spectators_unspawned_string
            : g_local_player_spectators_spawned_string;
        rf::gr::string_aligned(
            rf::gr::ALIGN_LEFT,
            x,
            y,
            text.c_str(),
            hud_get_default_font()
        );
    }
}

CallHook<void(int *dx, int *dy, int *dz)> control_config_get_mouse_delta_hook{
    0x0043D6D6,
    [] (int* const dx, int* const dy, int* dz) {
        // If active, do not write mouse wheel scroll delta.
        if (g_remote_server_cfg_popup.is_active()) {
            int tmp{};
            dz = &tmp;
        }
        control_config_get_mouse_delta_hook.call_target(dx, dy, dz);
    }
};

FunHook<void()> hud_msg_render_hook{
    0x004382D0,
    [] {
        if (!g_remote_server_cfg_popup.is_active()) {
            hud_msg_render_hook.call_target();
        }
    },
};

CodeInjection multi_hud_render_patch{
    0x00476D76,
    [] {
        if (g_remote_server_cfg_popup.is_active()) {
            g_remote_server_cfg_popup.render();
        }

        if (rf::gameseq_get_state() == rf::GS_MULTI_LIMBO) {
            return;
        }

        multi_hud_render_local_player_spectators();

        if (g_draw_vote_notification) {
            hud_render_vote_notification();
        }

        if (g_draw_ready_notification) {
            hud_render_ready_notification();
        }

        if (g_draw_respawn_timer_notification) {
            hud_render_respawn_timer_notification();
        }

        if (g_chat_menu_active != ChatMenuType::None) {
            hud_render_draw_chat_menu();

            if (!g_chat_menu_timer.valid() || g_chat_menu_timer.elapsed()) {
                toggle_chat_menu(ChatMenuType::None);
            }
        }
    }
};

void multi_hud_level_init() {
    g_draw_respawn_timer_notification = false;
    g_draw_respawn_timer_can_respawn = false;
    g_run_life_start_timestamp.invalidate();
    g_run_timer_reset_by_respawn_key = false;
    g_run_timer_fade_active = false;

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
            std::string msg = get_level_info_value<std::string>(id);
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

void draw_chat_menu_text(const int x, int y, const int w) {
    if (!g_active_menu) {
        return;
    }

    const int font_id = rf::gr::load_font(
        g_alpine_game_config.big_hud ? "regularfont.ttf:13" : "rfpc-medium.vf"
    );
    const int font_height = rf::gr::get_font_height(font_id);

    rf::gr::set_color(255, 255, 180, 0xCC);
    rf::gr::string_aligned(
        rf::gr::ALIGN_LEFT,
        x + 8,
        y + 4,
        g_active_menu->display_string.c_str(),
        font_id
    );

    y += font_height * 2;

    int display_index = 1;
    for (const ChatMenuElement& element : g_active_menu->elements) {
        // If the element is not valid for this game mode, skip it
        // completely (no blank slot)
        if (!is_element_valid(element)) {
            y += font_height;
            ++display_index; // Skip this number
            continue;
        }

        const std::string_view display_name = element.display_string.empty()
            ? element.long_string
            : element.display_string;
        std::string line = std::format("{}: {}", display_index, display_name);
        gr_fit_string(line, w - 8 * 2, font_id);
        rf::gr::string_aligned(
            rf::gr::ALIGN_LEFT,
            x + 8,
            y + 4,
            line.c_str(),
            font_id
        );

        y += font_height;
        ++display_index;
    }

    // Add back or exit option
    rf::gr::string_aligned(
        rf::gr::ALIGN_LEFT,
        x + 8,
        y + 4,
        g_previous_menu ? "\n0: BACK\n" : "\n0: EXIT\n",
        font_id
    );
}

void hud_render_draw_chat_menu() {
    const int w = g_alpine_game_config.big_hud ? 281 : 200;
    const int h = g_alpine_game_config.big_hud ? 269 : 166;
    const int x = 10;
    const int y = (rf::gr::screen_height() - h) / 2;

    rf::gr::set_color(0, 0, 0, 0x80);
    rf::gr::rect(x, y, w, h);
    rf::gr::set_color(79, 216, 255, 0x80);
    rf::gr::rect_border(x, y, w, h);

    draw_chat_menu_text(x, y, w);
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
                g_active_menu = multi_is_team_game_type() ? &radio_messages_menu : &express_menu;
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
                send_chat_line_packet(msg, nullptr);
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

FunHook<void(const rf::Player*, const char*, int)> chat_add_msg_hook{
    0x00443FB0,
    [] (const rf::Player* const player, const char* const msg, const int message_type) {
        handle_sound_msg(msg);
        chat_add_msg_hook.call_target(player, msg, message_type);
    },
};

static bool g_target_player_is_bot = false;

// displays name of player you're pointing at
CallHook<void(int, int, const char*, int, rf::gr::Mode)> multi_hud_render_target_player_name_hook{
    0x00478140,
    [] (int x, int y, const char* s, int font_num, rf::gr::Mode mode) {
        if (!g_alpine_game_config.display_target_player_names) {
            return;
        }
        multi_hud_render_target_player_name_hook.call_target(x, y, s, font_num, mode);
        if (g_target_player_is_bot) {
            const rf::gr::Color saved_color = rf::gr::screen.current_color;
            rf::gr::set_color(255, 250, 205, 255);
            rf::gr::string(rf::gr::current_string_x, y, " bot", font_num, mode);
            rf::gr::set_color(saved_color);
        }
    },
};

CodeInjection multi_hud_update_target_player_patch_1{
    0x00477F06,
    [] (const auto& regs) {
        const rf::Player& player = addr_as_ref<rf::Player>(regs.esi - 8);
        g_target_player_is_bot = player.is_bot;
    },
};

CodeInjection multi_hud_update_target_player_patch_2{
    0x00477F8C,
    [] (const auto& regs) {
        const rf::Player& player = addr_as_ref<rf::Player>(regs.edi - 8);
        g_target_player_is_bot = player.is_bot;
    },
};

ConsoleCommand2 ui_playernames_cmd{
    "ui_playernames",
    []() {
        g_alpine_game_config.display_target_player_names = !g_alpine_game_config.display_target_player_names;
        rf::console::print("Display of names of targeted players is {}", g_alpine_game_config.display_target_player_names ? "enabled" : "disabled");
    },
    "Toggle displaying names of targeted players",
    "ui_playernames",
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
    rf::Color render_color = time_left_string_color;
    render_color.alpha = static_cast<uint8_t>(time_left_string_color.alpha * (rf::time_left_alpha / 255.0f));
    rf::gr::set_color(render_color);

    int x_pos = rf::gr::clip_width() - time_left_string_x_pos_offset;
    int y_pos = rf::gr::clip_height() - time_left_string_y_pos_offset;

    rf::gr::string_aligned(rf::gr::ALIGN_LEFT, x_pos, y_pos, time_left_string.c_str(), 0, rf::gr::text_2d_mode);
    },
};

CodeInjection multi_hud_handle_final_countdown_injection {
    0x00476EC5,
    []() {
        const int min_plus_sec = rf::time_left_minutes * 60 + rf::time_left_seconds;

        if (min_plus_sec > 60)
            rf::played_one_minute_left_sound = 0;
        if (min_plus_sec > 30)
            rf::played_half_minute_left_sound = 0;
        if (min_plus_sec > 10)
            memset(&rf::played_n_seconds_left_sound[0], 0, 10);
    }
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

    const auto [format_text_width, format_text_height] =
        rf::gr::get_string_size(time_left_string_format, 0);

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

    multi_hud_update_timer_color();
}

ConsoleCommand2 ui_verbosetimer_cmd{
    "ui_verbosetimer",
    []() {
        g_alpine_game_config.verbose_time_left_display = !g_alpine_game_config.verbose_time_left_display;
        build_time_left_string_format();
        rf::console::print("Verbose in-game timer display is {}", g_alpine_game_config.verbose_time_left_display ? "enabled" : "disabled");
    },
    "Control whether the in-game timer displays the 'Time Left:' text",
    "ui_verbosetimer",
};

ConsoleCommand2 ui_runtimer_cmd{
    "ui_runtimer",
    [] {
        g_alpine_game_config.show_run_timer = !g_alpine_game_config.show_run_timer;
        rf::console::print("RUN game type timer display is {}", g_alpine_game_config.show_run_timer ? "enabled" : "disabled"
        );
    },
    "Toggle the RUN game type timer display",
    "ui_runtimer",
};

ConsoleCommand2 ui_always_show_specators_cmd{
    "ui_always_show_specators",
    [] {
        g_alpine_game_config.always_show_spectators = !g_alpine_game_config.always_show_spectators;
        rf::console::print(
            "Always show spectators is {}",
            g_alpine_game_config.always_show_spectators ? "enabled" : "disabled"
        );
    },
    "Toggle display of spectators on your HUD",
    "ui_always_show_specators",
};

ConsoleCommand2 ui_simple_server_chat_messages_cmd{
    "ui_simple_server_chat_messages",
    [] {
        g_alpine_game_config.simple_server_chat_msgs =
            !g_alpine_game_config.simple_server_chat_msgs;
        rf::console::print(
            "Simple server chat messages is {}",
            g_alpine_game_config.simple_server_chat_msgs ? "enabled" : "disabled"
        );
    },
    "Toggle simple server chat messages",
    "ui_simple_server_chat_messages",
};

void multi_hud_apply_patches()
{
    hud_msg_render_hook.install();
    multi_hud_render_patch.install();
    AsmWriter{0x00477790}.jmp(multi_hud_render_team_scores);
    multi_hud_render_team_scores_new_gamemodes_patch.install();
    multi_powerup_render_gr_bitmap_hook.install();
    multi_hud_render_level_info_hook.install();
    multi_hud_init_hook.install();

    // Drawing of targeted player names in multi
    multi_hud_render_target_player_name_hook.install();
    multi_hud_update_target_player_patch_1.install();
    multi_hud_update_target_player_patch_2.install();

    constexpr float multi_hud_target_player_name_alpha = 255.f;
    write_mem<float>(0x00478021 + 6, multi_hud_target_player_name_alpha);

    // Play radio message and taunt sounds
    chat_add_msg_hook.install();

    // Draw Time Left label
    multi_hud_render_time_left_hook.install();

    // Reset final countdown sounds (allows "one minute remaining", etc. to play again if map is extended)
    multi_hud_handle_final_countdown_injection.install();

    // Console commands
    ui_playernames_cmd.register_cmd();
    ui_verbosetimer_cmd.register_cmd();
    ui_runtimer_cmd.register_cmd();
    ui_always_show_specators_cmd.register_cmd();
    ui_simple_server_chat_messages_cmd.register_cmd();

    control_config_get_mouse_delta_hook.install();
}

void multi_hud_on_local_spawn()
{
    if (gt_is_run() && !g_run_life_start_timestamp.valid()) {
        g_run_life_start_timestamp.set(0);
        if (g_run_timer_reset_by_respawn_key) {
            g_run_timer_fade_active = true;
        }
        g_run_timer_reset_by_respawn_key = false;
    }
}

void multi_hud_reset_run_gt_timer(bool triggered_by_respawn_key)
{
    g_run_life_start_timestamp.invalidate();
    g_run_timer_reset_by_respawn_key = triggered_by_respawn_key;
    g_run_timer_fade_active = false;
}

void multi_hud_set_big(bool is_big)
{
    g_big_team_scores_hud = is_big;
}
