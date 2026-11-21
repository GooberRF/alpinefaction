#pragma once

#include <cstddef>
#include <cstdint>
#include <patch_common/MemUtils.h>
#include "gr/gr_font.h"
#include "gr/gr.h"
#include "multi.h"
#include "os/array.h"

namespace rf::ui
{
    struct Gadget
    {
        void **vtbl;
        Gadget *parent;
        bool highlighted;
        bool enabled;
        int x;
        int y;
        int w;
        int h;
        int key;
        void(*on_click)(int x, int y);
        void(*on_mouse_btn_down)(int x, int y);

        [[nodiscard]] int get_absolute_x() const
        {
            return AddrCaller{0x004569E0}.this_call<int>(this);
        }

        [[nodiscard]] int get_absolute_y() const
        {
            return AddrCaller{0x00456A00}.this_call<int>(this);
        }

        void unhighlight()
        {
            AddrCaller{0x00440970}.this_call<void>(this);
        }

        void highlight()
        {
            AddrCaller{0x00436DF0}.this_call<void>(this);
        }

        void disable()
        {
            AddrCaller{0x00448A00}.this_call<void>(this);
        }
    };
    static_assert(sizeof(Gadget) == 0x28);

    struct Button : Gadget
    {
        int bg_bitmap;
        int selected_bitmap;
#ifdef ALPINE_FACTION
        int reserved0;
        char* text;
        int font;
        int reserved1[2];
#else
        int text_user_bmap;
        int text_offset_x;
        int text_offset_y;
        int text_width;
        int text_height;
#endif
        void create(const char* normal_bm_name, const char* select_bitmap_name, int x, int y, int Id, const char* title, int font_num)
        {
            AddrCaller{0x004574D0}.this_call<void>(this, normal_bm_name, select_bitmap_name, x, y, Id, title, font_num);
        }

        void render()
        {
            AddrCaller{0x004577A0}.this_call<void>(this);
        }

        void init()
        {
            AddrCaller{0x004574B0}.this_call<void>(this);
        }
    };
    static_assert(sizeof(Button) == 0x44);

    struct Slider : Gadget
    {
        bool vertical;
        int bm_w;
        int bm_h;
        float value;
        int bm_handle;
        int bm_handle_on;
        float min_value;
        float max_value;

        void create(rf::ui::Gadget* parent, const char* texture, const char* texture_slider_on, int x, int y, int w, int h, float min, float max)
        {
            AddrCaller{0x00457BD0}.this_call<void>(this, parent, texture, texture_slider_on, x, y, w, h, min, max);
        }

        void render()
        {
            AddrCaller{0x00457CA0}.this_call<void>(this);
        }

        void update_value(int x, int y)
        {
            AddrCaller{0x00457E10}.this_call<void>(this, x, y);
        }

        float get_value()
        {
            return AddrCaller{0x00457ED0}.this_call<float>(this);
        }

        void set_value(float value)
        {
            AddrCaller{0x00457EB0}.this_call<void>(this, value);
        }
    };
    static_assert(sizeof(Slider) == 0x48);

    struct Panel : Gadget
    {
        int bg_bm;

        void create(const char* normal_bm_name, int x, int y)
        {
            AddrCaller{0x00456A40}.this_call<void>(this, normal_bm_name, x, y);
        }

        void render()
        {
            AddrCaller{0x00456A80}.this_call<void>(this);
        }
    };
    static_assert(sizeof(Panel) == 0x2C);

    struct Container : Gadget
    {
        void *gadgets[32];
        int count;

        void add_gadget(Gadget* gadget)
        {
            AddrCaller{0x00458300}.this_call<void>(this, gadget);
        }
    };
    static_assert(sizeof(Container) == 0xAC);

    struct Checkbox : Button
    {
        int checked_bm;
        bool checked;

        void create(const char* normal_bm_name, const char* select_bm_name, const char* checked_bm_name, int x, int y, int a7, const char* text, int font)
        {
            AddrCaller{0x00457960}.this_call<void>(this, normal_bm_name, select_bm_name, checked_bm_name, x, y, a7, text, font);
        }

        void render()
        {
            AddrCaller{0x004579C0}.this_call<void>(this);
        }
    };
    static_assert(sizeof(Checkbox) == 0x4C);

    struct Label : Gadget
    {
        Color clr;
        int bitmap;
#ifdef ALPINE_FACTION
        char* text;
        int font;
        gr::TextAlignment align;
#else
        int text_offset_x;
        int text_offset_y;
        int text_width;
#endif
        int text_height;

        void init()
        {
            AddrCaller{0x00456B30}.this_call<void>(this);
        }

        void create(Gadget* parent, int x, int y, const char* text, int font)
        {
            AddrCaller{0x00456B60}.this_call<void>(this, parent, x, y, text, font);
        }

        void create2(Gadget* parent, int x, int y, int w, int h, const char* text, int font)
        {
            AddrCaller{0x00456C20}.this_call<void>(this, parent, x, y, w, h, text, font);
        }

        void render()
        {
            AddrCaller{0x00456ED0}.this_call<void>(this);
        }
    };
    static_assert(sizeof(Label) == 0x40);

    struct InputBox : Label
    {
        char text[32];
        int max_text_width;
        int font;
    };
    static_assert(sizeof(InputBox) == 0x68);

    struct Cycler : Gadget
    {
        static constexpr int max_items = 16;
        int item_text_bitmaps[max_items];
#ifdef ALPINE_FACTION
        char* items_text[max_items];
        int items_font[max_items];
#else
        int item_text_offset_x[max_items];
        int item_text_offset_y[max_items];
#endif
        int items_width[max_items];
        int items_height[max_items];
        int num_items;
        int current_item;
    };
    static_assert(sizeof(Cycler) == 0x170);

    static auto& popup_message = addr_as_ref<void(const char *title, const char *text, void(*callback)(), bool input)>(0x004560B0);
    using PopupCallbackPtr = void (*)();
    static auto& popup_custom =
        addr_as_ref<void(const char *title, const char *text, int num_btns, const char *choices[],
                       PopupCallbackPtr choices_callbacks[], int default_choice, int keys[])>(0x004562A0);
    static auto& popup_abort = addr_as_ref<void()>(0x004559C0);
    static auto& popup_set_text = addr_as_ref<void(const char *text)>(0x00455A50);
    static auto& popup_get_input = addr_as_ref<void(char* string, int max_len)>(0x004566A0);
    static auto& popup_is_active = addr_as_ref<bool()>(0x00456680);
    // A popup button and what its on_click runs once it has closed the popup.
    struct PopupButton
    {
        Button button;
        PopupCallbackPtr callback;
    };
    static_assert(sizeof(PopupButton) == 0x48);
    static_assert(offsetof(PopupButton, callback) == 0x44);
    static auto& popup_title = addr_as_ref<Label>(0x006428F0);
    // popup_set_text breaks the text into these.
    static auto& popup_text_lines = addr_as_ref<Label[3]>(0x00642978);
    // Enabled only for popup_message's input form.
    static auto& popup_input_box = addr_as_ref<InputBox>(0x00642A68);
    // What Enter presses: a button's index, or popup_input_gadget, which presses button 0.
    static auto& popup_current_gadget = addr_as_ref<int>(0x00642AE8);
    inline constexpr int popup_input_gadget = 4;
    // Filled from 0; the rest are disabled.
    static auto& popup_buttons = addr_as_ref<PopupButton[4]>(0x00642B20);

    static auto& mainmenu_quit_game_confirmed = addr_as_ref<void()>(0x00443CB0);
    // The main menu's buttons in stock's column order, which is mainmenu_button_action's numbering.
    enum MainMenuButton
    {
        MAIN_MENU_BUTTON_NEW_GAME = 0, // with its training choice
        MAIN_MENU_BUTTON_SAVE = 1,
        MAIN_MENU_BUTTON_LOAD = 2,
        MAIN_MENU_BUTTON_MULTI = 3,
        MAIN_MENU_BUTTON_OPTIONS = 4,
        MAIN_MENU_BUTTON_RETURN = 5,
        MAIN_MENU_BUTTON_EXTRAS = 6,
        MAIN_MENU_BUTTON_QUIT = 7, // with its confirmation
    };
    static auto& mainmenu_button_action = addr_as_ref<void(MainMenuButton button)>(0x004438C0);
    // New Game's choices: each starts a new game at its level, l1s1.rfl or train01.rfl.
    static auto& mainmenu_new_game_skip_training = addr_as_ref<void()>(0x00443B10);
    static auto& mainmenu_new_game_training = addr_as_ref<void()>(0x00443A80);
    // The main menu's MULTIPLAYER and EXTRAS buttons: each queues its state. The first also ends a game that
    // is not beneath the menu.
    static auto& mainmenu_open_multi_menu = addr_as_ref<void()>(0x00443C20);
    static auto& mainmenu_open_extras = addr_as_ref<void()>(0x00443C70);
    // The MULTIPLAYER button's action without its sound: the network checks, then mainmenu_open_multi_menu, or a
    // popup in its place: an error, or one whose continue button calls it.
    static auto& mainmenu_multi_click = addr_as_ref<void()>(0x00443B70);
    // Set by the main menu's init while a game runs beneath it, which turns it into the in-game menu.
    static auto& mainmenu_game_beneath = addr_as_ref<bool>(0x0063C11C);
    static auto& menu_draw_background = addr_as_ref<void()>(0x00442CE0);
    static auto& menu_draw_side_bars = addr_as_ref<void()>(0x00442DB0);
    static auto& menu_background_bitmap = addr_as_ref<int>(0x00598FEC); // mars_background.tga, -1 if missing
    static auto& menu_background_scroll_speed = addr_as_ref<float>(0x00598FFC); // texels per second, 15.0
    // The menu music. level_load sets main_menu_music_stopped, and the main menu's init then restarts it.
    static auto& main_menu_music_filename = addr_as_ref<String>(0x0063C110);
    static auto& main_menu_music_sig = addr_as_ref<int>(0x005990F8);
    static auto& main_menu_music_stopped = addr_as_ref<int>(0x005990FC);
    static auto& main_menu_music_play = addr_as_ref<bool(const char* filename)>(0x00443230);
    static auto& main_menu_music_stop = addr_as_ref<void()>(0x00443E90);

    // Stock Extras button on_click handlers.
    static auto& extras_credits_click = addr_as_ref<void(int x, int y)>(0x0043EC70);
    static auto& extras_summoner_trailer_click = addr_as_ref<void(int x, int y)>(0x0043EC80);
    static auto& extras_glass_house_click = addr_as_ref<void(int x, int y)>(0x0043ECA0);

    static auto& get_gadget_from_pos = addr_as_ref<int(int x, int y, Gadget * const gadgets[], int num_gadgets)>(0x00442ED0);
    static auto& update_input_box_cursor = addr_as_ref<void()>(0x00456960);

    static auto& scale_x = addr_as_ref<float>(0x00598FB8);
    static auto& scale_y = addr_as_ref<float>(0x00598FBC);
    static auto& input_box_cursor_visible = addr_as_ref<bool>(0x00642DC8);
    static auto& large_font = addr_as_ref<int>(0x0063C05C);
    static auto& medium_font_0 = addr_as_ref<int>(0x0063C060);
    static auto& medium_font_1 = addr_as_ref<int>(0x0063C064);
    static auto& small_font = addr_as_ref<int>(0x0063C068);

    static auto& video_gamma_slider = addr_as_ref<Slider>(0x00640C10);

    static auto& audio_options_panel = addr_as_ref<Panel>(0x006424D8);
    static auto& audio_sfx_slider = addr_as_ref<Slider>(0x00642418);
    static auto& audio_sfx_slider_on_click = addr_as_ref<void(int x, int y)>(0x00454070);
    static auto& audio_music_slider = addr_as_ref<Slider>(0x006423D0);
    static auto& audio_music_slider_on_click = addr_as_ref<void(int x, int y)>(0x004540E0);
    static auto& audio_message_slider = addr_as_ref<Slider>(0x00642490);
    static auto& audio_message_slider_on_click = addr_as_ref<void(int x, int y)>(0x00454150);
    static auto& audio_sfx_button = addr_as_ref<Button>(0x006421E0);
    static auto& audio_music_button = addr_as_ref<Button>(0x00642098);
    static auto& audio_message_button = addr_as_ref<Button>(0x00642228);

    // options
    static auto& menu_main_buttons_y = addr_as_ref<int>(0x00598FCC);
    static auto& menu_button_offset_y = addr_as_ref<int>(0x00598FD0);
    static auto& options_menu_offset = addr_as_ref<float>(0x0063F8D8);
    static auto& options_menu_tab_move_anim_speed = addr_as_ref<float>(0x0063F930);
    static auto& options_current_panel = addr_as_ref<int>(0x0059A5D4);
    static auto& menu_move_anim_speed = addr_as_ref<float>(0x00598FD4);
    static auto& options_incoming_panel = addr_as_ref<int>(0x0059A5D8);
    static auto& options_close_current_panel = addr_as_ref<int()>(0x0044F8D0);
    static auto& options_set_panel_open = addr_as_ref<void()>(0x0044F8C0);
    static auto& options_panel_x = addr_as_ref<int>(0x0063C058);
    static auto& options_panel_y = addr_as_ref<int>(0x00598FE0);
    static auto& options_animated_offset = addr_as_ref<float>(0x0063FA14);
    static auto& options_buttons = addr_as_ref<Gadget*[5]>(0x0063FB6C); // Game, Video, Audio, Controls, Back
    static auto& options_is_panel_open = addr_as_ref<bool>(0x0063F920);

    // options -> game
    static auto& options_game_autoswitch_priority_up_on_click = addr_as_ref<void(int x, int y)>(0x00452E90);
    static auto& options_game_autoswitch_priority_down_on_click = addr_as_ref<void(int x, int y)>(0x00452EE0);
    static auto& options_game_current_gadget = addr_as_ref<int>(0x0064204C);

    // options -> controls
    static auto& options_controls_bindings_up_on_click = addr_as_ref<void(int x, int y)>(0x004500E0);
    static auto& options_controls_bindings_down_on_click = addr_as_ref<void(int x, int y)>(0x00450130);
    static auto& options_controls_current_gadget = addr_as_ref<int>(0x0063FFE8);
    static auto& options_controls_waiting_for_key = addr_as_ref<bool>(0x00640060);
    // Assigns to the selected binding row and clears the same scan code / mouse button from every
    // other binding (pass -1 for the unused parameter).
    static auto& options_controls_assign_binding = addr_as_ref<void(int scan_code, int mouse_btn_id)>(0x0044FFD0);
    static auto& options_controls_stop_waiting_for_key = addr_as_ref<void()>(0x0044FC00);

    // multi
    // The JOIN GAME button: queues GS_MULTI_SERVER_LIST.
    static auto& multi_menu_join_game_on_click = addr_as_ref<void()>(0x00448B70);

    // multi -> create game
    static auto& create_game_map_list_up_on_click = addr_as_ref<void(int x, int y)>(0x004451F0);
    static auto& create_game_map_list_down_on_click = addr_as_ref<void(int x, int y)>(0x00445260);
    static auto& create_game_options_current_gadget = addr_as_ref<int>(0x0063CA8C);
    static auto& create_game_current_tab = addr_as_ref<int>(0x0063F850);
    // The level name in an RFL's header; false when the file is not a level. The callee destroys `filename`.
    static auto& create_game_get_level_name = addr_as_ref<bool(String::Pod filename, String& name)>(0x00446140);

    // multi -> join server (server browser)
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

    enum ServerListRecordFlags : uint32_t
    {
        SERVER_LIST_RECORD_DEDICATED = 0x1,
        SERVER_LIST_RECORD_PASSWORD = 0x2,
    };

    // A favlist.adr / svrlist.adr record (multi_join_game_get_server_data 0x0044ADA0, read and written by
    // multi_join_game_load_list 0x0044B610 / multi_join_game_save_list 0x0044BA60).
#pragma pack(push, 1)
    struct ServerListRecord
    {
        uint32_t ip; // host byte order
        uint16_t port;
        uint16_t padding;
        char name[32];
        char level[32];
        char mod[16];
        int32_t game_type;
        uint8_t players;
        uint8_t max_players;
        int16_t ping;
        uint32_t flags; // ServerListRecordFlags
    };
#pragma pack(pop)
    static_assert(sizeof(ServerListRecord) == 100);
    static_assert(offsetof(ServerListRecord, port) == 0x04);
    static_assert(offsetof(ServerListRecord, name) == 0x08);
    static_assert(offsetof(ServerListRecord, level) == 0x28);
    static_assert(offsetof(ServerListRecord, mod) == 0x48);
    static_assert(offsetof(ServerListRecord, game_type) == 0x58);
    static_assert(offsetof(ServerListRecord, players) == 0x5C);
    static_assert(offsetof(ServerListRecord, max_players) == 0x5D);
    static_assert(offsetof(ServerListRecord, ping) == 0x5E);
    static_assert(offsetof(ServerListRecord, flags) == 0x60);

    static auto& join_game_server_list_up_on_click = addr_as_ref<void(int x, int y)>(0x0044B4C0);
    static auto& join_game_server_list_down_on_click = addr_as_ref<void(int x, int y)>(0x0044B520);
    static auto& join_game_current_gadget = addr_as_ref<int>(0x0063EE78);
    static auto& server_browser_server_list = addr_as_ref<ServerListEntry*>(0x0063F62C);
    static auto& server_browser_sel_idx = addr_as_ref<int>(0x00599CDC);
    static auto& server_browser_num_servers = addr_as_ref<int>(0x0063EE74);
    static auto& server_browser_sorted_indices = addr_as_ref<int*>(0x0063F634);
    static auto& server_browser_num_sorted_indices = addr_as_ref<int>(0x0063F578);
    static auto& server_browser_sel_sorted_idx = addr_as_ref<int>(0x0063F644);
    static auto& server_browser_sort_column = addr_as_ref<int>(0x00599D20);
    static auto& server_browser_filter_type = addr_as_ref<int>(0x0063F22C);
    static auto& server_browser_filter_cycler = addr_as_ref<Cycler>(0x0063F0C0);

    // A saved game in the Load Game and Save Game lists, which list the savegame folder's .svl files newest first.
    struct SavegameListEntry
    {
        char filename[260]; // with its extension
        int write_time;     // a 32-bit time_t
        float game_time;    // seconds
    };
    static_assert(sizeof(SavegameListEntry) == 0x10C);
    static_assert(offsetof(SavegameListEntry, write_time) == 0x104);
    static_assert(offsetof(SavegameListEntry, game_time) == 0x108);

    // load game
    static auto& load_game_up_on_click = addr_as_ref<void(int x, int y)>(0x0043F8C0);
    static auto& load_game_down_on_click = addr_as_ref<void(int x, int y)>(0x0043F940);
    static auto& load_game_current_gadget = addr_as_ref<int>(0x0063B810);
    // Appends the saved games whose header reads; the caller empties the list first.
    static auto& load_game_build_list = addr_as_ref<void()>(0x004406D0);
    static auto& load_game_list = addr_as_ref<VArray<SavegameListEntry>>(0x0063B9B0);
    static auto& load_game_sel_idx = addr_as_ref<int>(0x0063B9C8);
    // Loads the selected entry when its header reads: its level to load, then GS_NEW_LEVEL.
    static auto& load_game_load_click = addr_as_ref<void()>(0x0043F5F0);

    // save game
    static auto& save_game_up_on_click = addr_as_ref<void(int x, int y)>(0x00441240);
    static auto& save_game_down_on_click = addr_as_ref<void(int x, int y)>(0x004412D0);
    static auto& save_game_current_gadget = addr_as_ref<int>(0x0063BCB8);
    // As load_game_build_list.
    static auto& save_game_build_list = addr_as_ref<void()>(0x00440D00);
    static auto& save_game_list = addr_as_ref<VArray<SavegameListEntry>>(0x0063BCC0);

    // message log panel
    static auto& message_log_up_on_click = addr_as_ref<void(int x, int y)>(0x00454F80);
    static auto& message_log_down_on_click = addr_as_ref<void(int x, int y)>(0x00454FD0);
    static auto& message_log_current_gadget = addr_as_ref<int>(0x006428D8);

}
