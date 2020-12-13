#include "player.h"
#include "../rf/player.h"
#include "../rf/entity.h"
#include "../rf/multi.h"
#include "../console/console.h"
#include "../main.h"
#include "../multi/multi.h"
#include "../in_game_ui/spectate_mode.h"
#include <common/utils/list-utils.h>
#include <common/config/GameConfig.h>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>

std::unordered_map<rf::Player*, PlayerAdditionalData> g_player_additional_data_map;

void find_player(const StringMatcher& query, std::function<void(rf::Player*)> consumer)
{
    auto player_list = SinglyLinkedList{rf::player_list};
    for (auto& player : player_list) {
        if (query(player.name))
            consumer(&player);
    }
}

PlayerAdditionalData& get_player_additional_data(rf::Player* player)
{
    return g_player_additional_data_map[player];
}

FunHook<rf::Player*(bool)> player_create_hook{
    0x004A3310,
    [](bool is_local) {
        rf::Player* player = player_create_hook.call_target(is_local);
        multi_init_player(player);
        return player;
    },
};

FunHook<void(rf::Player*)> player_destroy_hook{
    0x004A35C0,
    [](rf::Player* player) {
        spectate_mode_on_destroy_player(player);
        player_destroy_hook.call_target(player);
        g_player_additional_data_map.erase(player);
    },
};

FunHook<rf::Entity*(rf::Player*, int, const rf::Vector3*, const rf::Matrix3*, int)> player_create_entity_hook{
    0x004A4130,
    [](rf::Player* pp, int entity_type, const rf::Vector3* pos, const rf::Matrix3* orient, int multi_entity_index) {
        auto ep = player_create_entity_hook.call_target(pp, entity_type, pos, orient, multi_entity_index);
        if (ep) {
            spectate_mode_player_create_entity_post(pp, ep);
        }
        return ep;
    },
};

bool should_swap_weapon_alt_fire(rf::Player* player)
{
    if (!g_game_config.swap_assault_rifle_controls) {
        return false;
    }

    auto entity = rf::entity_from_handle(player->entity_handle);
    if (!entity) {
        return false;
    }

    // Check if local entity is attached to a parent (vehicle or torret)
    auto parent = rf::entity_from_handle(entity->host_handle);
    if (parent) {
        return false;
    }

    static auto& assault_rifle_cls_id = addr_as_ref<int>(0x00872470);
    return entity->ai.current_primary_weapon == assault_rifle_cls_id;
}

bool is_player_weapon_on(rf::Player* player, bool alt_fire) {
    if (!player) {
        player = rf::local_player;
    }
    auto entity = rf::entity_from_handle(player->entity_handle);
    bool is_continous_fire = rf::entity_weapon_is_on(entity->handle, entity->ai.current_primary_weapon);
    bool is_alt_fire_flag_set = (entity->ai.flags & 0x2000) != 0; // EWF_ALT_FIRE = 0x2000
    if (should_swap_weapon_alt_fire(player)) {
        is_alt_fire_flag_set = !is_alt_fire_flag_set;
    }
    return is_continous_fire && is_alt_fire_flag_set == alt_fire;
}

FunHook<void(rf::Player*, bool, bool)> player_fire_primary_weapon_hook{
    0x004A4E80,
    [](rf::Player* player, bool alt_fire, bool was_pressed) {
        if (should_swap_weapon_alt_fire(player)) {
            alt_fire = !alt_fire;
        }
        player_fire_primary_weapon_hook.call_target(player, alt_fire, was_pressed);
    },
};

CodeInjection stop_continous_primary_fire_patch{
    0x00430EC5,
    [](auto& regs) {
        auto entity = reinterpret_cast<rf::Entity*>(regs.esi);
        if (is_player_weapon_on(entity->local_player, false)) {
            regs.eip = 0x00430EDF;
        }
        else {
            regs.eip = 0x00430EF2;
        }
    }
};

CodeInjection stop_continous_alternate_fire_patch{
    0x00430F09,
    [](auto& regs) {
        auto entity = reinterpret_cast<rf::Entity*>(regs.esi);
        if (is_player_weapon_on(entity->local_player, true)) {
            regs.eip = 0x00430F23;
        }
        else {
            regs.eip = 0x00430F36;
        }
    }
};

ConsoleCommand2 swap_assault_rifle_controls_cmd{
    "swap_assault_rifle_controls",
    []() {
        g_game_config.swap_assault_rifle_controls = !g_game_config.swap_assault_rifle_controls;
        g_game_config.save();
        rf::console_printf("Swap assault rifle controls: %s",
                     g_game_config.swap_assault_rifle_controls ? "enabled" : "disabled");
    },
    "Swap Assault Rifle controls",
};

FunHook<void(rf::Player*, int)> player_make_weapon_current_selection_hook{
    0x004A4980,
    [](rf::Player* player, int weapon_type) {
        player_make_weapon_current_selection_hook.call_target(player, weapon_type);
        auto entity = rf::entity_from_handle(player->entity_handle);
        if (entity && rf::is_multi) {
            // Reset impact delay timers when switching weapon (except in SP because of speedrunners)
            entity->ai.create_weapon_delay_timestamps[0].invalidate();
            entity->ai.create_weapon_delay_timestamps[1].invalidate();
        }
    },
};

void __fastcall player_execute_action_timestamp_set_new(rf::Timestamp* fire_wait_timer, int, int value)
{
    if (!fire_wait_timer->valid() || fire_wait_timer->time_until() < value) {
        fire_wait_timer->set(value);
    }
}

CallHook<void __fastcall(rf::Timestamp*, int, int)> player_execute_action_timestamp_set_fire_wait_patch{
    {0x004A62C2u, 0x004A6325u},
    &player_execute_action_timestamp_set_new,
};

FunHook<void(rf::Player*, rf::ControlAction, bool)> player_execute_action_hook{
    0x004A6210,
    [](rf::Player* player, rf::ControlAction ctrl, bool was_pressed) {
        if (!spectate_mode_handle_ctrl_in_game(ctrl, was_pressed)) {
            player_execute_action_hook.call_target(player, ctrl, was_pressed);
        }
    },
};

FunHook<bool(rf::Player*)> player_is_local_hook{
    0x004A68D0,
    [](rf::Player* player) {
        if (spectate_mode_is_active()) {
            return false;
        }
        return player_is_local_hook.call_target(player);
    }
};

bool player_is_dead_and_not_spectating(rf::Player* player)
{
    if (spectate_mode_is_active())
        return false;
    else
        return rf::player_is_dead(player);
}

CallHook player_is_dead_red_bars_hook{0x00432A52, player_is_dead_and_not_spectating};
CallHook player_is_dead_scoreboard_hook{0x00437BEE, player_is_dead_and_not_spectating};
CallHook player_is_dead_scoreboard2_hook{0x00437C25, player_is_dead_and_not_spectating};

static bool player_is_dying_and_not_spectating(rf::Player* player)
{
    if (spectate_mode_is_active())
        return false;
    else
        return rf::player_is_dying(player);
}

CallHook player_is_dying_red_bars_hook{0x00432A5F, player_is_dying_and_not_spectating};
CallHook player_is_dying_scoreboard_hook{0x00437C01, player_is_dying_and_not_spectating};
CallHook player_is_dying_scoreboard2_hook{0x00437C36, player_is_dying_and_not_spectating};

void player_do_patch()
{
    // general hooks
    player_create_hook.install();
    player_destroy_hook.install();

    // Allow swapping Assault Rifle primary and alternate fire controls
    player_fire_primary_weapon_hook.install();
    stop_continous_primary_fire_patch.install();
    stop_continous_alternate_fire_patch.install();
    swap_assault_rifle_controls_cmd.register_cmd();

    // Reset impact delay timers when switching weapon to avoid delayed fire after switching
    player_make_weapon_current_selection_hook.install();

    // Fix setting fire wait timer when closing weapon switch menu
    // Note: this timer makes sense for weapons that require holding (not clicking) the control to fire (e.g. shotgun)
    player_execute_action_timestamp_set_fire_wait_patch.install();

    // spectate mode support
    player_execute_action_hook.install();
    player_create_entity_hook.install();
    player_is_local_hook.install();

    player_is_dying_red_bars_hook.install();
    player_is_dying_scoreboard_hook.install();
    player_is_dying_scoreboard2_hook.install();

    player_is_dead_red_bars_hook.install();
    player_is_dead_scoreboard_hook.install();
    player_is_dead_scoreboard2_hook.install();

    // Increase damage for kill command in Single Player
    write_mem<float>(0x004A4DF5 + 1, 100000.0f);

    // Allow undefined mp_character in player_create_entity
    // Fixes Go_Undercover event not changing player 3rd person character
    AsmWriter(0x004A414F, 0x004A4153).nop();
}