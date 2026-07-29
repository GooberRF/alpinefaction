#include <common/utils/list-utils.h>
#include <patch_common/FunHook.h>
#include <patch_common/ShortTypes.h>
#include <patch_common/AsmWriter.h>
#include <unordered_map>
#include "multi.h"
#include "gametype.h"
#include "endgame_votes.h"
#include "../main/main.h"
#include "server.h"
#include "../os/console.h"
#include "../rf/player/player.h"
#include "../rf/entity.h"
#include "../rf/localize.h"
#include "../rf/multi.h"
#include "../rf/weapon.h"
#include "../hud/multi_spectate.h"
#include "../hud/hud_internal.h"
#include "../misc/alpine_settings.h"
#include "../sound/sound.h"
#include "server_internal.h"
#include "multi_private.h"
#include "mutators.h"
#include "alpine_packets.h"
#include "sprays.h"
#include "gungame.h"
#include "../misc/player.h"
#include "../misc/misc.h"
#include "kill.h"

bool kill_messages = true;

void player_fpgun_on_player_death(rf::Player* pp);

void multi_kill_init_player(rf::Player* player)
{
    auto* stats = static_cast<PlayerStatsNew*>(player->stats);
    stats->clear();
}

FunHook<void()> multi_level_init_hook{
    0x0046E450,
    [] {
        for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
            multi_kill_init_player(&player);
            if (rf::is_server) {
                player.death_time.reset();
                if (player.is_bot) {
                    player.is_spawn_disabled = true;
                }
            }
        }

        multi_level_init_hook.call_target();

        // Clear all sprays on both client and server when a new level loads.
        sprays_level_init();

        // Stop allowing endgame votes after the next level starts
        multi_player_set_can_endgame_vote(false);

        // Re-evaluate footstep state when level loads
        evaluate_footsteps();
    },
};

static const char* null_to_empty(const char* str)
{
    return str ? str : "";
}

void print_kill_message(rf::Player* killed_player, rf::Player* killer_player)
{
    rf::String msg;
    const char* mui_msg;
    rf::ChatMsgColor color_id;

    rf::Entity* killer_entity = killer_player ? rf::entity_from_handle(killer_player->entity_handle) : nullptr;

    if (!killer_player) {
        color_id = rf::ChatMsgColor::default_;
        mui_msg = null_to_empty(rf::strings::was_killed_mysteriously);
        msg = rf::String::format("{}{}", killed_player->name, mui_msg);
    }
    else if (killed_player == rf::local_player) {
        color_id = rf::ChatMsgColor::white_white;
        if (killer_player == killed_player) {
            mui_msg = null_to_empty(rf::strings::you_killed_yourself);
            msg = rf::String::format("{}", mui_msg);
        }
        else if (killer_entity && killer_entity->ai.current_primary_weapon == rf::riot_stick_weapon_type) {
            mui_msg = null_to_empty(rf::strings::you_just_got_beat_down_by);
            msg = rf::String::format("{}{}!", mui_msg, killer_player->name);
        }
        else {
            mui_msg = null_to_empty(rf::strings::you_were_killed_by);

            auto& killer_name = killer_player->name;
            int killer_weapon_cls_id = killer_entity ? killer_entity->ai.current_primary_weapon : -1;
            if (killer_weapon_cls_id >= 0 && killer_weapon_cls_id < 64) {
                auto& weapon_cls = rf::weapon_types[killer_weapon_cls_id];
                auto& weapon_name = weapon_cls.display_name;
                msg = rf::String::format("{}{}'s {}!", mui_msg, killer_name, string_to_lower(weapon_name));
            }
            else {
                msg = rf::String::format("{}{}!", mui_msg, killer_name);
            }
        }
    }
    else if (killer_player == rf::local_player) {
        color_id = rf::ChatMsgColor::white_white;
        mui_msg = null_to_empty(rf::strings::you_killed);
        msg = rf::String::format("{}{}!", mui_msg, killed_player->name);
    }
    else {
        rf::Player* spectate_target = multi_spectate_is_following_player() ? multi_spectate_get_target_player() : nullptr;
        color_id = (killed_player == spectate_target || killer_player == spectate_target)
            ? rf::ChatMsgColor::white_white : rf::ChatMsgColor::default_;
        if (killer_player == killed_player) {
            if (rf::multi_entity_is_female(killed_player->settings.multi_character))
                mui_msg = null_to_empty(rf::strings::was_killed_by_her_own_hand);
            else
                mui_msg = null_to_empty(rf::strings::was_killed_by_his_own_hand);
            msg = rf::String::format("{}{}", killed_player->name, mui_msg);
        }
        else {
            if (killer_entity && killer_entity->ai.current_primary_weapon == rf::riot_stick_weapon_type)
                mui_msg = null_to_empty(rf::strings::got_beat_down_by);
            else
                mui_msg = null_to_empty(rf::strings::was_killed_by);
            msg = rf::String::format("{}{}{}", killed_player->name, mui_msg, killer_player->name);
        }
    }

    if (g_alpine_game_config.killfeed_enabled) {
        bool is_team_mode = multi_is_team_game_type();
        rf::Player* spectate_target = multi_spectate_is_following_player() ? multi_spectate_get_target_player() : nullptr;
        bool is_local = (killed_player == rf::local_player || killer_player == rf::local_player
                         || killed_player == spectate_target || killer_player == spectate_target);

        if (is_local) {
            // Local player involved: show full message in white
            killfeed_add_kill(nullptr, 0, nullptr, 0, msg.c_str(), true, is_team_mode);
        }
        else if (!killer_player) {
            // Mysterious death: "PlayerName was killed mysteriously"
            killfeed_add_kill(killed_player->name, killed_player->team,
                              nullptr, 0,
                              null_to_empty(rf::strings::was_killed_mysteriously),
                              false, is_team_mode);
        }
        else if (killer_player == killed_player) {
            // Self-kill: "PlayerName was killed by his/her own hand"
            const char* self_verb;
            if (rf::multi_entity_is_female(killed_player->settings.multi_character))
                self_verb = null_to_empty(rf::strings::was_killed_by_her_own_hand);
            else
                self_verb = null_to_empty(rf::strings::was_killed_by_his_own_hand);
            killfeed_add_kill(killed_player->name, killed_player->team,
                              nullptr, 0,
                              self_verb, false, is_team_mode);
        }
        else {
            // Third-party kill: "KilledName verb KillerName"
            const char* verb;
            if (killer_entity && killer_entity->ai.current_primary_weapon == rf::riot_stick_weapon_type)
                verb = null_to_empty(rf::strings::got_beat_down_by);
            else
                verb = null_to_empty(rf::strings::was_killed_by);
            killfeed_add_kill(killed_player->name, killed_player->team,
                              killer_player->name, killer_player->team,
                              verb, false, is_team_mode);
        }
    }
    else {
        rf::String prefix;
        rf::multi_chat_print(msg, color_id, prefix);
    }
}

void distribute_effective_health(rf::Entity* ep, float amount, float max_life_cap, float max_armor_cap)
{
    if (!ep || amount <= 0.0f) return;

    const float life_to_add = std::min(amount, std::max(0.0f, max_life_cap - ep->life));
    const float armor_to_add = std::min((amount - life_to_add) / 2.0f, std::max(0.0f, max_armor_cap - ep->armor));

    ep->life += life_to_add;
    ep->armor += armor_to_add;
}

void multi_apply_kill_reward(rf::Player* player)
{
    rf::Entity* ep = rf::entity_from_handle(player->entity_handle);
    if (!ep) {
        return;
    }

    const auto& conf = g_alpine_server_config_active_rules.kill_rewards;

    // Ensure that max health/armor limits do not decrease current values
    const float max_life_limit = std::max(ep->life, conf.kill_reward_health_super ? 200.0f : ep->info->max_life);
    const float max_armor_limit = std::max(ep->armor, conf.kill_reward_armor_super ? 200.0f : ep->info->max_armor);

    // Apply health reward, ensuring we do not exceed max limits
    if (conf.kill_reward_health > 0.0f) {
        ep->life = std::min(ep->life + conf.kill_reward_health, max_life_limit);
    }

    // Apply armor reward, ensuring we do not exceed max limits
    if (conf.kill_reward_armor > 0.0f) {
        ep->armor = std::min(ep->armor + conf.kill_reward_armor, max_armor_limit);
    }

    // Apply effective health reward, distributed between health and armor
    if (conf.kill_reward_effective_health > 0.0f) {
        distribute_effective_health(ep, conf.kill_reward_effective_health,
                                     max_life_limit, max_armor_limit);
    }
}

void on_player_kill(rf::Player* killed_player, rf::Player* killer_player)
{
    if (kill_messages) {
        print_kill_message(killed_player, killer_player);
    }

    update_player_active_status(killed_player); // active pulse on killed

    if (rf::is_server) {
        killed_player->death_time.emplace(std::chrono::steady_clock::now());
    }

    auto* killed_stats = static_cast<PlayerStatsNew*>(killed_player->stats);
    killed_stats->inc_deaths();

    if (killer_player) {
        auto* killer_stats = static_cast<PlayerStatsNew*>(killer_player->stats);
        const bool score_from_kills = !gt_uses_custom_scoring();
        if (killer_player != killed_player) {
            if (score_from_kills) {
                rf::player_add_score(killer_player, 1);
            }
            killer_stats->inc_kills();
        }
        else {
            if (score_from_kills) {
                rf::player_add_score(killer_player, -1);

                // decrement TDM team score on self kill in match mode servers
                if (g_alpine_server_config.vote_match.enabled
                    && rf::multi_get_game_type() == rf::NG_TYPE_TEAMDM) {
                    multi_tdm_add_team_score(killer_player, -1);
                }
            }
        }

        multi_apply_kill_reward(killer_player);

        // Arena mutator: instantly top up the killer's current weapon after a frag.
        if (killer_player != killed_player) {
            mutators_on_player_frag(killer_player);
        }

        multi_spectate_on_player_kill(killed_player, killer_player);

        if (gt_is_gungame() && killer_player != killed_player) {
            gungame_on_player_kill(killer_player, killed_player);
        }
    }
}

FunHook<void(rf::Entity*)> entity_on_death_hook{
    0x0041FDC0,
    [](rf::Entity* entity) {
        // Reset fpgun animation when player dies
        if (rf::local_player && entity->handle == rf::local_player->entity_handle && rf::local_player->weapon_mesh_handle) {
            player_fpgun_on_player_death(rf::local_player);
        }
        entity_on_death_hook.call_target(entity);
    },
};

ConsoleCommand2 kill_messages_cmd{
    "kill_messages",
    []() {
        kill_messages = !kill_messages;
    },
    "Toggles printing of kill messages in the chatbox and the game console",
};

void multi_kill_do_patch()
{
    // Player kill handling
    using namespace asm_regs;
    AsmWriter(0x00420703)
        .push(ebx)
        .push(edi)
        .call(on_player_kill)
        .add(esp, 8)
        .jmp(0x00420B03);

    // Change player stats structure
    write_mem<i8>(0x004A33B5 + 1, sizeof(PlayerStatsNew));
    multi_level_init_hook.install();

    // Reset fpgun animation when player dies
    entity_on_death_hook.install();

    // Allow disabling kill messages
    kill_messages_cmd.register_cmd();
}
