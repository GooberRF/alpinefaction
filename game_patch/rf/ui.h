#pragma once

#include <patch_common/MemUtils.h>
#include "gr/gr_font.h"
#include "gr/gr.h"

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
#ifdef DASH_FACTION
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
#ifdef DASH_FACTION
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
#ifdef DASH_FACTION
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

    static auto& mainmenu_quit_game_confirmed = addr_as_ref<void()>(0x00443CB0);

    static auto& get_gadget_from_pos = addr_as_ref<int(int x, int y, Gadget * const gadgets[], int num_gadgets)>(0x00442ED0);
    static auto& update_input_box_cursor = addr_as_ref<void()>(0x00456960);

    static auto& scale_x = addr_as_ref<float>(0x00598FB8);
    static auto& scale_y = addr_as_ref<float>(0x00598FBC);
    static auto& input_box_cursor_visible = addr_as_ref<bool>(0x00642DC8);
    static auto& large_font = addr_as_ref<int>(0x0063C05C);
    static auto& medium_font_0 = addr_as_ref<int>(0x0063C060);
    static auto& medium_font_1 = addr_as_ref<int>(0x0063C064);
    static auto& small_font = addr_as_ref<int>(0x0063C068);

    // options menu globals
    static auto& g_MenuMainButtonsY = *reinterpret_cast<int*>(0x00598FCC);
    static auto& menu_button_offset_y = *reinterpret_cast<int*>(0x00598FD0);
    static auto& g_fOptionsMenuOffset = *reinterpret_cast<float*>(0x0063F8D8);
    static auto& options_menu_tab_move_anim_speed = addr_as_ref<float>(0x0063F930);
    static auto& options_current_panel = addr_as_ref<int>(0x0059A5D4);
    static auto& menu_move_anim_speed = addr_as_ref<float>(0x00598FD4);
    static auto& options_current_panel_id = addr_as_ref<int>(0x0059A5D8);
    static auto& options_close_current_panel = addr_as_ref<int()>(0x0044F8D0);
    static auto& options_set_panel_open = addr_as_ref<void()>(0x0044F8C0);
    static auto& options_panel_x = addr_as_ref<int>(0x0063C058);
    static auto& options_panel_y = addr_as_ref<int>(0x00598FE0);
    static auto& options_animated_offset = addr_as_ref<float>(0x0063FA14);
    static auto& options_back_button = addr_as_ref<Button>(0x0063FB28);
}
