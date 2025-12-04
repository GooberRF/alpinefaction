#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <common/version/version.h>
#include <xlog/xlog.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>
#include "../rf/ui.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/input.h"
#include "../rf/file/file.h"
#include "../rf/multi.h"
#include "../rf/sound/sound.h"
#include "../rf/sound/sound_ds.h"
#include "../rf/os/frametime.h"
#include "../rf/os/os.h"
#include "../main/main.h"
#include "../graphics/gr.h"
#include "../os/os.h"
#include "../input/input.h"
#include "misc.h"
#include "alpine_settings.h"

constexpr int EGG_ANIM_ENTER_TIME = 2000;
constexpr int EGG_ANIM_LEAVE_TIME = 2000;
constexpr int EGG_ANIM_IDLE_TIME = 3000;

constexpr double PI = 3.14159265358979323846;

static int g_version_click_counter = 0;
static int g_egg_anim_start;
static int g_game_music_sig_to_restore = -1;
static int g_game_music_start_sig = -1;

namespace rf
{
    static auto& menu_version_label = addr_as_ref<ui::Gadget>(0x0063C088);
    static auto& main_menu_music_sig = addr_as_ref<int>(0x005990F8);
}

struct Snowflake
{
    float x;
    float y;
    float speed;
    float sway_phase;
    float sway_speed;
    float size;
};

static std::vector<Snowflake> g_menu_snowflakes;

static bool is_snow_season()
{
    const std::time_t time_now = std::time(nullptr);
    const std::tm date_now = *std::localtime(&time_now);

    const int month = date_now.tm_mon;
    const int day = date_now.tm_mday;

    return (month == 11 && day >= 11 && day <= 31); // Dec 11 - 31
}

static void reset_menu_snowflakes()
{
    g_menu_snowflakes.clear();
}

static void ensure_menu_snowflakes()
{
    if (!g_menu_snowflakes.empty())
        return;

    constexpr int flake_count = 100;
    std::uniform_real_distribution<float> x_dist(0.0f, static_cast<float>(rf::gr::screen_width()));
    std::uniform_real_distribution<float> y_dist(0.0f, static_cast<float>(rf::gr::screen_height()));
    std::uniform_real_distribution<float> speed_dist(35.0f, 90.0f);
    std::uniform_real_distribution<float> sway_speed_dist(1.5f, 3.5f);
    std::uniform_real_distribution<float> size_dist(1.5f, 6.5f);

    g_menu_snowflakes.reserve(flake_count);
    for (int i = 0; i < flake_count; ++i) {
        g_menu_snowflakes.push_back({
            x_dist(g_rng),
            y_dist(g_rng),
            speed_dist(g_rng),
            std::uniform_real_distribution<float>(0.0f, static_cast<float>(2 * PI))(g_rng),
            sway_speed_dist(g_rng),
            size_dist(g_rng),
        });
    }
}

static void update_and_render_menu_snow()
{
    const float screen_w = static_cast<float>(rf::gr::screen_width());
    const float screen_h = static_cast<float>(rf::gr::screen_height());
    const float dt = rf::frametime;

    ensure_menu_snowflakes();

    rf::gr::set_color(255, 255, 255, 200);

    for (auto& flake : g_menu_snowflakes) {
        flake.y += flake.speed * dt;
        flake.sway_phase += flake.sway_speed * dt;
        flake.x += std::sin(flake.sway_phase) * 20.0f * dt;

        if (flake.y > screen_h) {
            flake.y = -flake.size;
            flake.x = std::uniform_real_distribution<float>(0.0f, screen_w)(g_rng);
        }

        if (flake.x < -flake.size) {
            flake.x = screen_w + flake.size;
        }
        else if (flake.x > screen_w + flake.size) {
            flake.x = -flake.size;
        }

        int draw_x = static_cast<int>(flake.x);
        int draw_y = static_cast<int>(flake.y);
        int draw_size = std::max(1, static_cast<int>(flake.size));

        rf::gr::rect(draw_x, draw_y, draw_size, draw_size);
    }
}

// Note: fastcall is used because MSVC does not allow free thiscall functions
using UiLabel_Create2_Type = void __fastcall(rf::ui::Gadget*, int, rf::ui::Gadget*, int, int, int, int, const char*, int);
extern CallHook<UiLabel_Create2_Type> UiLabel_create2_version_label_hook;
void __fastcall UiLabel_create2_version_label(rf::ui::Gadget* self, int edx, rf::ui::Gadget* parent, int x, int y, int w,
                                             int h, const char* text, int font_id)
{
    // if a TC mod is loaded, show the mod name on the main menu
    std::string version_text = rf::mod_param.found()
        ? std::format("AF {} | {}", VERSION_STR, rf::mod_param.get_arg())
        : PRODUCT_NAME_VERSION;
    text = version_text.c_str();
    ui_get_string_size(&w, &h, text, -1, font_id);
    x = 430 - w;
    w += 5;
    h += 2;
    UiLabel_create2_version_label_hook.call_target(self, edx, parent, x, y, w, h, text, font_id);
}
CallHook<UiLabel_Create2_Type> UiLabel_create2_version_label_hook{0x0044344D, UiLabel_create2_version_label};

CallHook<void()> main_menu_process_mouse_hook{
    0x004437B9,
    []() {
        main_menu_process_mouse_hook.call_target();
        if (rf::mouse_was_button_pressed(0)) {
            int x, y, z;
            rf::mouse_get_pos(x, y, z);
            rf::ui::Gadget* gadgets_to_check[1] = {&rf::menu_version_label};
            int matched = rf::ui::get_gadget_from_pos(x, y, gadgets_to_check, std::size(gadgets_to_check));
            if (matched == 0) {
                xlog::trace("Version clicked");
                ++g_version_click_counter;
                if (g_version_click_counter == 3)
                    g_egg_anim_start = GetTickCount();
            }
        }
    },
};

int initiate_garden_king()
{
    int hbm = rf::bm::load("radar_dish.tga", -1, true);
    return hbm == -1 ? -1 : hbm;
}

CallHook<void()> main_menu_render_hook{
    0x00443802,
    []() {
        main_menu_render_hook.call_target();
        if (g_version_click_counter >= 3) {
            static int img = initiate_garden_king();
            if (img == -1)
                return;
            int w, h;
            rf::bm::get_dimensions(img, &w, &h);
            int anim_delta_time = GetTickCount() - g_egg_anim_start;
            int pos_x = (rf::gr::screen_width() - w) / 2;
            int pos_y = rf::gr::screen_height() - h;
            if (anim_delta_time < EGG_ANIM_ENTER_TIME) {
                float enter_progress = anim_delta_time / static_cast<float>(EGG_ANIM_ENTER_TIME);
                pos_y += h - static_cast<int>(sinf(enter_progress * static_cast<float>(PI) / 2.0f) * h);
            }
            else if (anim_delta_time > EGG_ANIM_ENTER_TIME + EGG_ANIM_IDLE_TIME) {
                int leave_delta = anim_delta_time - (EGG_ANIM_ENTER_TIME + EGG_ANIM_IDLE_TIME);
                float leave_progress = leave_delta / static_cast<float>(EGG_ANIM_LEAVE_TIME);
                pos_y += static_cast<int>((1.0f - cosf(leave_progress * static_cast<float>(PI) / 2.0f)) * h);
                if (leave_delta > EGG_ANIM_LEAVE_TIME)
                    g_version_click_counter = 0;
            }
            rf::gr::bitmap(img, pos_x, pos_y, rf::gr::bitmap_clamp_mode);
        }
    },
};

struct ServerListEntry
{
    char name[32];
    char level_name[32];
    char mod_name[16];
    int game_type;
    rf::NetAddr addr;
    char current_players;
    char max_players;
    int16_t ping;
    int field_60;
    char field_64;
    int flags;
};
static_assert(sizeof(ServerListEntry) == 0x6C, "invalid size");

FunHook<int(const int&, const int&)> server_list_cmp_func_hook{
    0x0044A6D0,
    [](const int& index1, const int& index2) {
        auto* server_list = addr_as_ref<ServerListEntry*>(0x0063F62C);
        bool has_ping1 = server_list[index1].ping >= 0;
        bool has_ping2 = server_list[index2].ping >= 0;
        if (has_ping1 != has_ping2) {
            return has_ping1 ? -1 : 1;
        }
        return server_list_cmp_func_hook.call_target(index1, index2);
    },
};

FunHook<void()> mainmenu_init_hook{
    0x00443270,
    []() {
        mainmenu_init_hook.call_target();
        apply_maximum_fps(); // set maximum FPS
        reset_menu_snowflakes();
    },
};

FunHook<void()> menu_draw_background_hook{
    0x00442CE0,
    []() {
        menu_draw_background_hook.call_target();

        if (g_alpine_game_config.seasonal_effect > 0) {
            if (g_alpine_game_config.seasonal_effect == 2 ||
                (g_alpine_game_config.seasonal_effect == 1 && is_snow_season())) {
                update_and_render_menu_snow();
            }
            else {
                reset_menu_snowflakes();
            }
        }
    },
};

CodeInjection menu_draw_background_injection{
    0x00442D5C,
    [](auto& regs) {
        auto& menu_background_bitmap = addr_as_ref<int>(0x00598FEC);
        auto& menu_background_x = addr_as_ref<float>(0x0063C074);

        rf::gr::set_color(255, 255, 255, 255);
        // Use function that accepts float sx
        //for (int i = 0; i < 100; ++i)
        gr_bitmap_scaled_float(menu_background_bitmap, 0.0f, 0.0f,
            static_cast<float>(rf::gr::screen.max_w), static_cast<float>(rf::gr::screen.max_h),
            menu_background_x, 0.0f, 640.0f, 480.0f, false, false, rf::gr::bitmap_clamp_mode);
        regs.eip = 0x00442D94;
    },
};

CodeInjection multi_servers_on_list_click_injection{
    0x0044B084,
    [](auto& regs) {
        // Edi register contains mouse click X position relative to the server list box left edge.
        // It is compared to hard-coded range of coordinates that designate area used by "fav" column.
        // Those hard-coded coordinates make sense only in 640x480 resolution (menu native resolution).
        // Because of that mouse click coordinate must be scaled from screen resolution to UI resolution before
        // comparision.
        int rel_y = regs.edi;
        rel_y = static_cast<int>(static_cast<float>(rel_y) / rf::ui::scale_x);
        regs.edi = rel_y;
    },
};

CodeInjection main_menu_set_music_injection{
    0x0044323C,
    []() {
        g_game_music_sig_to_restore = rf::snd_music_sig;
    },
};

FunHook<void()> main_menu_stop_music_hook{
    0x00443E90,
    []() {
        if (rf::snd_music_sig == rf::main_menu_music_sig) {
            main_menu_stop_music_hook.call_target();
            // Restore old sig in music sig variable so game can keep track of it (and stop it when needed)
            rf::snd_music_sig = g_game_music_sig_to_restore;
        }
        else if (g_game_music_sig_to_restore != -1) {
            // Music changed when the menu was open - stop the old music
            // Note: In Single Player RF pauses the world when entering the menu so it should not happen
            //       In Multi Player on the other hand level scripts are still executing in the background and nothing stops
            //       them from playing new sounds/music.
            rf::snd_pc_stop(g_game_music_sig_to_restore);
            g_game_music_sig_to_restore = -1;
        }
    },
};

CodeInjection game_music_play_hook{
    0x00434139,
    []() {
        if (rf::is_multi) g_game_music_start_sig = rf::snd_music_sig;
    },
};

CodeInjection snd_pause_multi_hook{
    0x00522C09,
    [](auto& regs) {
        int idx = regs.edi;
        if (rf::is_multi && g_game_music_start_sig != -1 && rf::ds_channels[idx].sig == g_game_music_start_sig) regs.eip = 0x00522C25;
    },
};

CodeInjection gameplay_close_vol_hook{
    0x0043175B,
    []() {
        if (rf::is_multi && g_game_music_start_sig != -1) rf::snd_ds_set_volume(g_game_music_start_sig, 0.0f);
    },
};

CodeInjection gameplay_init_vol_hook{
    0x00431684,
    []() {
        if (rf::is_multi && g_game_music_start_sig != -1) rf::snd_ds_set_volume(g_game_music_start_sig, rf::snd_group_volume[rf::SOUND_GROUP_MUSIC]);
    },
};

CodeInjection snd_music_update_volume_hook{
    0x00505E9E,
    [](auto& regs) {
        if (rf::is_multi && g_game_music_start_sig != -1 && regs.ecx == g_game_music_start_sig) regs.eip = 0x00505EA3;
    },
};

CodeInjection multi_create_game_do_frame_patch{
    0x0044EA8D,
    [](auto& regs) {
        // tab 0 = options
        // gadget 5 = map list
        if (rf::ui::create_game_current_tab == 0 && rf::ui::create_game_options_current_gadget == 5) {
            int mouse_dz = get_mouse_scroll_wheel_value();
            if (mouse_dz != 0) {
                mouse_dz > 0 ? rf::ui::create_game_map_list_up_on_click(-1, -1)
                             : rf::ui::create_game_map_list_down_on_click(-1, -1);
            }
        }
    },
};

CodeInjection multi_join_server_do_frame_patch{
    0x0044D16C,
    [](auto& regs) {
        // 3 = server list
        // 4, 5, 6 = scroll bar and buttons
        // 7, 8, 9, 10, 11, 12 = column headers
        if (rf::ui::join_game_current_gadget >= 3 && rf::ui::join_game_current_gadget <= 12) {
            int mouse_dz = get_mouse_scroll_wheel_value();
            if (mouse_dz != 0) {
                mouse_dz > 0 ? rf::ui::join_game_server_list_up_on_click(-1, -1)
                             : rf::ui::join_game_server_list_down_on_click(-1, -1);
            }
        }
    },
};

CodeInjection options_do_frame_patch{
    0x0044F211,
    [](auto& regs) {
        if (rf::ui::options_is_panel_open) {
            int mouse_dz = get_mouse_scroll_wheel_value();

            // 0 = game
            // 1 = video
            // 2 = audio
            // 3 = controls
            // 4 = advanced
            switch (rf::ui::options_current_panel) {
                case 0: { // game
                    // 4 = autoswitch priority list
                    if (rf::ui::options_game_current_gadget == 4) {
                        if (mouse_dz != 0) {
                            mouse_dz > 0 ? rf::ui::options_game_autoswitch_priority_up_on_click(-1, -1)
                                         : rf::ui::options_game_autoswitch_priority_down_on_click(-1, -1);
                        }
                    }

                    break;
                }

                case 3: { // controls
                    if (rf::ui::options_controls_waiting_for_key)
                        break; // do not scroll if waiting for an input to bind

                    // 2 = binds list
                    if (rf::ui::options_controls_current_gadget == 2) {
                        if (mouse_dz != 0) {
                            mouse_dz > 0 ? rf::ui::options_controls_bindings_up_on_click(-1, -1)
                                         : rf::ui::options_controls_bindings_down_on_click(-1, -1);
                        }
                    }

                    break;
                }

                default: {
                    break;
                }
            }
        }
    },
};

CodeInjection game_load_do_frame_patch{
    0x004408E1,
    [](auto& regs) {
        // 0 = save list
        // 1, 2, 3 = scroll bar and buttons
        if (rf::ui::load_game_current_gadget >= 0 && rf::ui::load_game_current_gadget <= 3) {
            int mouse_dz = get_mouse_scroll_wheel_value();
            if (mouse_dz != 0) {
                mouse_dz > 0 ? rf::ui::load_game_up_on_click(-1, -1)
                             : rf::ui::load_game_down_on_click(-1, -1);
            }
        }
    },
};

CodeInjection game_save_do_frame_patch{
    0x004422D1,
    [](auto& regs) {
        // 3 = save list
        // 4, 5, 6 = scroll bar and buttons
        if (rf::ui::save_game_current_gadget >= 3 && rf::ui::save_game_current_gadget <= 6) {
            int mouse_dz = get_mouse_scroll_wheel_value();
            if (mouse_dz != 0) {
                mouse_dz > 0 ? rf::ui::save_game_up_on_click(-1, -1)
                             : rf::ui::save_game_down_on_click(-1, -1);
            }
        }
    },
};

CodeInjection message_log_do_frame_patch{
    0x00455148,
    [](auto& regs) {
        // 1 = close button
        // 0 = scroll bar and buttons
        // list can't be selected directly, gadget -1 when not hovering close or scroll bar
        if (rf::ui::message_log_current_gadget != 1) {
            int mouse_dz = get_mouse_scroll_wheel_value();
            if (mouse_dz != 0) {
                mouse_dz > 0 ? rf::ui::message_log_up_on_click(-1, -1)
                             : rf::ui::message_log_down_on_click(-1, -1);
            }
        }
    },
};

void apply_main_menu_patches()
{
    // Main menu init
    mainmenu_init_hook.install();

    // Version in Main Menu
    UiLabel_create2_version_label_hook.install();
    main_menu_process_mouse_hook.install();
    main_menu_render_hook.install();

    // Put not responding servers at the bottom of server list
    server_list_cmp_func_hook.install();

    // Fix multi menu having background scroll speed doubled
    write_mem<int8_t>(0x00443C2E + 1, 0);
    write_mem<int8_t>(0x00443C30 + 1, 0);

    // Make menu background scrolling smooth on high resolutions
    menu_draw_background_injection.install();

    // Render seasonal effects
    menu_draw_background_hook.install();

    // Fix clicking fav checkbox in server list
    multi_servers_on_list_click_injection.install();

    // Fix music being unstoppable after opening the menu
    main_menu_set_music_injection.install();
    main_menu_stop_music_hook.install();

    // Fixes for MP music desync when going into the menu
    game_music_play_hook.install(); //save sig of last Music_Start
    snd_pause_multi_hook.install(); //don't pause last Music_Start sig when pausing all sounds
    gameplay_close_vol_hook.install(); //set last Music_Start sig volume to 0 in gameplay_close
    snd_music_update_volume_hook.install(); //don't update volume on last Music_Start sig in snd_music_update_volume
    gameplay_init_vol_hook.install(); //set last Music_Start sig volume back to what it should be in gameplay_init

    // Support scroll wheel in menu list boxes
    multi_create_game_do_frame_patch.install();
    multi_join_server_do_frame_patch.install();
    options_do_frame_patch.install();
    game_load_do_frame_patch.install();
    game_save_do_frame_patch.install();
    message_log_do_frame_patch.install();
}
