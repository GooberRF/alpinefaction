#include <common/utils/list-utils.h>
#include <patch_common/FunHook.h>
#include <patch_common/ShortTypes.h>
#include <patch_common/AsmWriter.h>
#include <algorithm>
#include <chrono>
#include <optional>
#include <span>
#include <string>
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
#include "kill_attribution.h"

bool kill_messages = true;

void player_fpgun_on_player_death(rf::Player* pp);

struct PendingKillAttribution
{
    KillAttribution attr;
    std::chrono::steady_clock::time_point received_at;
};

// The attribution is queued immediately ahead of obj_kill on the reliable stream, so
// anything appreciably older than that is stale and must not decorate a later death.
static constexpr std::chrono::seconds pending_attribution_lifetime{3};

static std::unordered_map<uint8_t, PendingKillAttribution> g_pending_kill_attributions;

void multi_kill_set_pending_attribution(const KillInfoPayload& payload,
                                        std::span<const uint8_t> assist_player_ids)
{
    PendingKillAttribution pending{};
    pending.attr.killer_player_id = payload.killer_player_id;
    pending.attr.weapon_type = payload.weapon_type;
    pending.attr.flags = payload.flags;
    pending.attr.damage_type = payload.damage_type;
    auto& assists = pending.attr.assist_player_ids;
    assists.reserve(std::min<size_t>(assist_player_ids.size(), af_kill_info_max_assists));
    for (uint8_t assist_id : assist_player_ids) {
        if (assists.size() >= af_kill_info_max_assists) {
            break;
        }
        if (std::find(assists.begin(), assists.end(), assist_id) == assists.end()) {
            assists.push_back(assist_id);
        }
    }
    pending.received_at = std::chrono::steady_clock::now();

    g_pending_kill_attributions[payload.killed_player_id] = std::move(pending);
}

static std::optional<KillAttribution> consume_pending_attribution(uint8_t killed_player_id)
{
    auto it = g_pending_kill_attributions.find(killed_player_id);
    if (it == g_pending_kill_attributions.end()) {
        return {};
    }

    std::optional<KillAttribution> attr;
    if (std::chrono::steady_clock::now() - it->second.received_at <= pending_attribution_lifetime) {
        attr = it->second.attr;
    }
    g_pending_kill_attributions.erase(it);
    return attr;
}

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

        // Drop kill attributions so a death at map end cannot decorate one on the next map.
        g_pending_kill_attributions.clear();
        kill_attribution_level_init();

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

// Attribution for the kill being printed, from the local record on the server and from the
// pending map on a client. Consumed on use, so a repeated obj_kill for the same victim falls
// back like any other kill the server said nothing about.
static std::optional<KillAttribution> get_kill_attribution(rf::Player* killed_player, rf::Player* killer_player)
{
    if (!rf::is_multi || !killed_player || !killed_player->net_data) {
        return {};
    }

    const uint8_t killed_id = killed_player->net_data->player_id;
    std::optional<KillAttribution> attr = rf::is_server
        ? kill_attribution_consume(killed_id)
        : consume_pending_attribution(killed_id);
    if (!attr) {
        return {};
    }

    // A record naming a different killer than the kill being printed is stale.
    const uint8_t killer_id = (killer_player && killer_player->net_data)
        ? killer_player->net_data->player_id : 0xFF;
    if (attr->killer_player_id != killer_id) {
        return {};
    }
    return attr;
}

// Lowercased display name of the attributed weapon, or empty when there is nothing worth
// showing (no attribution, a melee kill, or an index this build cannot resolve).
static std::string attribution_weapon_name(const std::optional<KillAttribution>& attr)
{
    if (!attr || (attr->flags & AF_KILL_FLAG_MELEE)) {
        return {};
    }
    const int weapon_type = attr->weapon_type;
    if (!kill_attribution_is_valid_weapon_type(weapon_type)) {
        return {};
    }
    return string_to_lower(rf::weapon_types[weapon_type].display_name);
}

// True when the server would have told us what killed this player, so silence is an answer
// rather than a gap. An Alpine 1.4+ server skips kill-info packets that carry nothing
// actionable, so on such a server a missing attribution means "the server had nothing to
// say".
// The rf::is_server term matters: on a dedicated or listen server the local attribution
// records *are* the authority, while get_af_server_info() is client-side state that may be
// unpopulated there. A client that has not received af_server_info yet is deliberately not
// authoritative, so it keeps the pre-1.4 behavior until it knows better.
static bool kill_attribution_server_is_authoritative()
{
    return rf::is_server || is_server_minimum_af_version(1, 4);
}

// Melee picks the verb. Prefer the server's attribution, fall back to the held-weapon
// heuristic only on a server that would not have told us either way.
static bool kill_was_melee(const std::optional<KillAttribution>& attr, rf::Entity* killer_entity)
{
    if (attr && attr->weapon_type != 0xFF) {
        return (attr->flags & AF_KILL_FLAG_MELEE) != 0;
    }
    if (kill_attribution_server_is_authoritative()) {
        // Every melee kill carries a real weapon type, so AF_KILL_FLAG_MELEE always rides
        // along with one and its payload is never skipped as empty. Reaching here on an
        // authoritative server therefore means this was not a melee kill - not that the
        // killer's currently held weapon should be consulted.
        return false;
    }
    if (!killer_entity) {
        return false;
    }
    const int held_weapon = killer_entity->ai.current_primary_weapon;
    if (held_weapon == rf::riot_stick_weapon_type) {
        return true;
    }
    const int riot_shield = kill_attribution_riot_shield_type();
    return riot_shield >= 0 && held_weapon == riot_shield;
}

// " (+ Name1, Name2)" for the assisting players still in the game, empty when none resolve.
static std::string assist_suffix(const std::optional<KillAttribution>& attr)
{
    if (!attr || attr->assist_player_ids.empty()) {
        return {};
    }

    std::string names;
    for (uint8_t assist_id : attr->assist_player_ids) {
        rf::Player* assister = rf::multi_find_player_by_id(assist_id);
        if (!assister) {
            continue; // left the game since the kill
        }
        if (!names.empty()) {
            names += ", ";
        }
        names += assister->name.c_str();
    }
    if (names.empty()) {
        return {};
    }
    return " (+ " + names + ")";
}

// True when `player` is credited with an assist on this kill. Assisters get the same line
// everyone else sees, just highlighted - there is deliberately no separate assist message.
static bool attribution_credits_player(const std::optional<KillAttribution>& attr, const rf::Player* player)
{
    if (!attr || !player || !player->net_data) {
        return false;
    }
    const uint8_t player_id = player->net_data->player_id;
    return std::find(attr->assist_player_ids.begin(), attr->assist_player_ids.end(), player_id)
        != attr->assist_player_ids.end();
}

// Assist highlighting applies only where the assist detail is actually shown.
static bool attribution_highlights_local(const std::optional<KillAttribution>& attr,
                                         bool is_third_party_kill, rf::Player* spectate_target)
{
    return is_third_party_kill
        && (attribution_credits_player(attr, rf::local_player)
            || attribution_credits_player(attr, spectate_target));
}

void print_kill_message(rf::Player* killed_player, rf::Player* killer_player)
{
    rf::String msg;
    const char* mui_msg;
    rf::ChatMsgColor color_id;

    rf::Entity* killer_entity = killer_player ? rf::entity_from_handle(killer_player->entity_handle) : nullptr;

    const std::optional<KillAttribution> attr = get_kill_attribution(killed_player, killer_player);
    const std::string attr_weapon_name = attribution_weapon_name(attr);
    const bool is_melee_kill = kill_was_melee(attr, killer_entity);

    // Trailing detail shared by the chat line and the killfeed segment.
    const bool is_third_party_kill = killer_player && killer_player != killed_player;
    // Assist credit goes on every non-suicide kill message, first person included. The weapon
    // clause is observer-only: the first-person lines already name the weapon themselves.
    const std::string assists_text = is_third_party_kill ? assist_suffix(attr) : std::string{};
    std::string kill_detail_suffix;
    if (is_third_party_kill) {
        if (!attr_weapon_name.empty()) {
            kill_detail_suffix = "'s " + attr_weapon_name;
        }
        kill_detail_suffix += assists_text;
    }

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
        else if (is_melee_kill) {
            mui_msg = null_to_empty(rf::strings::you_just_got_beat_down_by);
            msg = rf::String::format("{}{}{}!", mui_msg, killer_player->name, assists_text);
        }
        else {
            mui_msg = null_to_empty(rf::strings::you_were_killed_by);

            auto& killer_name = killer_player->name;
            if (!attr_weapon_name.empty()) {
                msg = rf::String::format("{}{}'s {}{}!", mui_msg, killer_name, attr_weapon_name,
                                         assists_text);
            }
            else if (attr) {
                // Server spoke and could not name a weapon: no weapon clause.
                msg = rf::String::format("{}{}{}!", mui_msg, killer_name, assists_text);
            }
            else if (kill_attribution_server_is_authoritative()) {
                // Silence from a 1.4+ server means it had nothing to say.
                msg = rf::String::format("{}{}{}!", mui_msg, killer_name, assists_text);
            }
            else {
                int killer_weapon_cls_id = killer_entity ? killer_entity->ai.current_primary_weapon : -1;
                if (killer_weapon_cls_id >= 0 && killer_weapon_cls_id < 64) {
                    auto& weapon_cls = rf::weapon_types[killer_weapon_cls_id];
                    auto& weapon_name = weapon_cls.display_name;
                    msg = rf::String::format("{}{}'s {}{}!", mui_msg, killer_name,
                                             string_to_lower(weapon_name), assists_text);
                }
                else {
                    msg = rf::String::format("{}{}{}!", mui_msg, killer_name, assists_text);
                }
            }
        }
    }
    else if (killer_player == rf::local_player) {
        color_id = rf::ChatMsgColor::white_white;
        mui_msg = null_to_empty(rf::strings::you_killed);
        msg = rf::String::format("{}{}{}!", mui_msg, killed_player->name, assists_text);
    }
    else {
        rf::Player* spectate_target = multi_spectate_is_following_player() ? multi_spectate_get_target_player() : nullptr;
        color_id = (killed_player == spectate_target || killer_player == spectate_target
                    || attribution_highlights_local(attr, is_third_party_kill, spectate_target))
            ? rf::ChatMsgColor::white_white : rf::ChatMsgColor::default_;
        if (killer_player == killed_player) {
            if (rf::multi_entity_is_female(killed_player->settings.multi_character))
                mui_msg = null_to_empty(rf::strings::was_killed_by_her_own_hand);
            else
                mui_msg = null_to_empty(rf::strings::was_killed_by_his_own_hand);
            msg = rf::String::format("{}{}", killed_player->name, mui_msg);
        }
        else {
            if (is_melee_kill)
                mui_msg = null_to_empty(rf::strings::got_beat_down_by);
            else
                mui_msg = null_to_empty(rf::strings::was_killed_by);
            msg = rf::String::format("{}{}{}{}", killed_player->name, mui_msg,
                                     killer_player->name, kill_detail_suffix);
        }
    }

    if (g_alpine_game_config.killfeed_enabled) {
        bool is_team_mode = multi_is_team_game_type();
        rf::Player* spectate_target = multi_spectate_is_following_player() ? multi_spectate_get_target_player() : nullptr;
        // An assister counts as involved: same line as everyone else, just in white.
        bool is_local = (killed_player == rf::local_player || killer_player == rf::local_player
                         || killed_player == spectate_target || killer_player == spectate_target
                         || attribution_highlights_local(attr, is_third_party_kill, spectate_target));

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
            if (is_melee_kill)
                verb = null_to_empty(rf::strings::got_beat_down_by);
            else
                verb = null_to_empty(rf::strings::was_killed_by);
            killfeed_add_kill(killed_player->name, killed_player->team,
                              killer_player->name, killer_player->team,
                              verb, false, is_team_mode,
                              kill_detail_suffix.empty() ? nullptr : kill_detail_suffix.c_str());
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

    // If an auto team balance is queued, swap this player over when they die on
    // the larger team. Runs after update_player_active_status above so the dead
    // player still counts toward their team's size.
    auto_team_balance_on_player_death(killed_player);
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
