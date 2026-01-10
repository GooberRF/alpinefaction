#pragma once

#include <patch_common/MemUtils.h>
#include "../os/string.h"

namespace rf
{
    struct PlayerSettings;

    enum ControlConfigAction
    {
        CC_ACTION_PRIMARY_ATTACK = 0x0,
        CC_ACTION_SECONDARY_ATTACK = 0x1,
        CC_ACTION_USE = 0x2,
        CC_ACTION_JUMP = 0x3,
        CC_ACTION_CROUCH = 0x4,
        CC_ACTION_HIDE_WEAPON = 0x5,
        CC_ACTION_RELOAD = 0x6,
        CC_ACTION_NEXT_WEAPON = 0x7,
        CC_ACTION_PREV_WEAPON = 0x8,
        CC_ACTION_CHAT = 0x9,
        CC_ACTION_TEAM_CHAT = 0xA,
        CC_ACTION_FORWARD = 0xB,
        CC_ACTION_BACKWARD = 0xC,
        CC_ACTION_SLIDE_LEFT = 0xD,
        CC_ACTION_SLIDE_RIGHT = 0xE,
        CC_ACTION_SLIDE_UP = 0xF,
        CC_ACTION_SLIDE_DOWN = 0x10,
        CC_ACTION_LOOK_DOWN = 0x11,
        CC_ACTION_LOOK_UP = 0x12,
        CC_ACTION_TURN_LEFT = 0x13,
        CC_ACTION_TURN_RIGHT = 0x14,
        CC_ACTION_MESSAGES = 0x15,
        CC_ACTION_MP_STATS = 0x16,
        CC_ACTION_QUICK_SAVE = 0x17,
        CC_ACTION_QUICK_LOAD = 0x18,
    };

    enum AlpineControlConfigAction
    {
        AF_ACTION_FLASHLIGHT = 0x0,
        AF_ACTION_SKIP_CUTSCENE = 0x1,
        AF_ACTION_SELF_KILL = 0x2,
        AF_ACTION_VOTE_YES = 0x3,
        AF_ACTION_VOTE_NO = 0x4,
        AF_ACTION_READY = 0x5,
        AF_ACTION_DROP_FLAG = 0x6,
		AF_ACTION_CHAT_MENU = 0x7,
        AF_ACTION_TAUNT_MENU = 0x8,
        AF_ACTION_COMMAND_MENU = 0x9,
        AF_ACTION_PING_LOCATION = 0xA,
        AF_ACTION_SPECTATE_MENU = 0xB,
        AF_ACTION_NO_AUTOSWITCH = 0xC,
        AF_ACTION_REMOTE_SERVER_CFG = 0xD,
        AF_ACTION_INSPECT_WEAPON = 0xE,
        _AF_ACTION_LAST_VARIANT = AF_ACTION_INSPECT_WEAPON
    };

    struct ControlConfigItem
    {
        int16_t default_scan_codes[2];
        int16_t default_mouse_btn_id;
        int16_t field_6;
        int is_repeat;
        String name;
        int16_t scan_codes[2];
        int16_t mouse_btn_id;
    };
    static_assert(sizeof(ControlConfigItem) == 0x1C);

    struct ControlAxisConfig
    {
        bool active;
        bool invert;
        int mouse_axis;
        int field_8;
        int field_C;
    };
    static_assert(sizeof(ControlAxisConfig) == 0x10);

    struct ControlConfig
    {
        float mouse_sensitivity;
        bool mouse_look;
        int current_delta_z;
        ControlConfigItem bindings[128];
        int num_bindings;
        ControlAxisConfig axes[4];
    };
    static_assert(sizeof(ControlConfig) == 0xE50);

    static auto& control_config_check_pressed =
        addr_as_ref<bool(ControlConfig *cc, ControlConfigAction action, bool *just_pressed)>(0x0043D4F0);
    static auto& control_is_control_down =
        addr_as_ref<bool(ControlConfig* ccp, ControlConfigAction action)>(0x00430F40);
    static auto& control_config_get_key_name = addr_as_ref<int(String *out, int key)>(0x0043D930);
    static auto& control_config_add_item = addr_as_ref<void(ControlConfig* ccp, const char* name,
        int is_repeat, int16_t key1, int16_t key2, int16_t mouse_button)>(0x0043CFD0);
    static auto& control_config_get_mouse_button_name = addr_as_ref<int(String *out, int button)>(0x0043D970);
    static auto& control_config_find_action_by_name = addr_as_ref<int(PlayerSettings *ps, const char *name)>(0x0043D9F0);
    static auto& control_config_get_num_bindings = addr_as_ref<int(ControlConfig* ai)>(0x0043D920);

}
