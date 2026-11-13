#include <algorithm>
#include <map>
#include <common/utils/list-utils.h>
#include <patch_common/CodeInjection.h>
#include "per_player_respawn.h"
#include "alpine_packets.h"
#include "server_internal.h"
#include "../graphics/gr_ghost_mesh.h"
#include "../rf/clutter.h"
#include "../rf/geometry.h"
#include "../rf/item.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../rf/vmesh.h"

namespace
{
    constexpr float ghost_alpha_below = 0.55f;
    constexpr float ghost_alpha_above = 0.15f;

    struct ItemGhost
    {
        rf::Timestamp until;
        int duration_ms = 0;
    };

    // Client: item handle -> the local player's cooldown on that copy.
    std::map<int, ItemGhost> g_ghosts;

    int respawn_ms_for(const rf::Item* item)
    {
        const auto& timeout_ms = g_alpine_server_config_active_rules.per_player_respawn_time_ms;
        return std::min(timeout_ms.value_or(item->respawn_time_ms), per_player_respawn_max_ms);
    }

    // A detail room is drawn inside the room it hangs off, so that is the pass its ghost joins.
    rf::GRoom* ghost_render_room(rf::GRoom* room)
    {
        if (room && room->is_detail && room->room_to_render_with) {
            return room->room_to_render_with;
        }
        return room;
    }
}

bool weapon_stay_is_exempt(int weapon_type)
{
    const auto& exemptions = g_alpine_server_config_active_rules.weapon_stay_exemptions.exemptions;
    return std::any_of(exemptions.begin(), exemptions.end(), [weapon_type](const WeaponStayExemptionEntry& e) {
        return e.exemption_enabled && e.index == weapon_type;
    });
}

bool per_player_respawn_applies(const rf::Item* item)
{
    if (!rf::is_server || g_alpine_server_config_active_rules.weapons_stay != WeaponStayMode::PerPlayer
        || !(rf::netgame.flags & rf::NG_FLAG_WEAPON_STAY) || !item || !item->info) {
        return false;
    }
    const int weapon_type = item->info->gives_weapon_id;
    return weapon_type >= 0 && !(item->item_flags & rf::IF_DROPPED) && !weapon_stay_is_exempt(weapon_type);
}

bool per_player_respawn_is_blocked(rf::Player* player, const rf::Item* item)
{
    auto it = player->item_respawn_cooldowns.find(item->handle);
    if (it == player->item_respawn_cooldowns.end()) {
        return false;
    }
    if (it->second.elapsed()) {
        player->item_respawn_cooldowns.erase(it);
        return false;
    }
    return true;
}

bool per_player_respawn_blocks(rf::Player* player, const rf::Item* item)
{
    if (!player || !item) {
        return false;
    }
    if (rf::is_server) {
        return per_player_respawn_is_blocked(player, item);
    }
    return player == rf::local_player && per_player_respawn_is_ghosted(item);
}

void per_player_respawn_on_picked_up(rf::Player* player, rf::Item* item)
{
    if (!player || !per_player_respawn_applies(item)) {
        return;
    }
    const int cooldown_ms = respawn_ms_for(item);
    if (cooldown_ms <= 0) {
        return;
    }
    player->item_respawn_cooldowns[item->handle].set(cooldown_ms);
    af_send_per_player_respawn(player, item->handle, cooldown_ms);
}

void per_player_respawn_on_player_killed(rf::Player* player)
{
    if (!player) {
        return;
    }
    if (rf::is_server) {
        player->item_respawn_cooldowns.clear();
    }
    if (player == rf::local_player) {
        g_ghosts.clear();
    }
}

void per_player_respawn_level_init()
{
    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        player.item_respawn_cooldowns.clear();
    }
    g_ghosts.clear();
}

void per_player_respawn_client_on_taken(int item_handle, int remaining_ms)
{
    if (remaining_ms <= 0 || !rf::item_from_handle(item_handle)) {
        return;
    }
    ItemGhost& ghost = g_ghosts[item_handle];
    ghost.until.set(remaining_ms);
    ghost.duration_ms = remaining_ms;
}

bool per_player_respawn_is_ghosted(const rf::Item* item)
{
    if (g_ghosts.empty() || !item) {
        return false;
    }
    auto it = g_ghosts.find(item->handle);
    return it != g_ghosts.end() && !it->second.until.elapsed();
}

void per_player_respawn_render_pass(rf::GRoom* room_filter)
{
    if (g_ghosts.empty() || !rf::is_multi || rf::is_dedicated_server || rf::monitor_render_in_progress) {
        return;
    }
    for (auto it = g_ghosts.begin(); it != g_ghosts.end();) {
        rf::Item* item = rf::item_from_handle(it->first);
        if (!item || it->second.until.elapsed()) {
            it = g_ghosts.erase(it);
            continue;
        }
        const ItemGhost& ghost = it->second;
        ++it;
        if (!item->vmesh || (item->obj_flags & (rf::OF_DELAYED_DELETE | rf::OF_HIDDEN))
            || ghost_render_room(item->room) != room_filter) {
            continue;
        }

        rf::Matrix3 orient = item->orient;
        if (item->info && (item->info->flags & rf::IIF_SPINS_IN_MULTI)) {
            // Mirror the stock renderer, which draws a spinning item from spin_angle.
            orient.set_from_angles(0.0f, 0.0f, -item->spin_angle);
        }
        rf::Vector3 bbox_min;
        rf::Vector3 bbox_max;
        rf::vmesh_get_bbox(item->vmesh, &bbox_min, &bbox_max);
        // The fill rises through the mesh as the cooldown runs down.
        const float progress = ghost.duration_ms > 0
            ? std::clamp(1.0f - static_cast<float>(ghost.until.time_until()) / ghost.duration_ms, 0.0f, 1.0f)
            : 1.0f;
        const float fill_y = item->pos.y + bbox_min.y + progress * (bbox_max.y - bbox_min.y);
        gr::render_ghost_mesh(item->vmesh, item->pos, orient, ghost_alpha_below, ghost_alpha_above, fill_y,
                              nullptr);
    }
}

// item_render: 0x00458FA2 is the first instruction of the multiplayer-only CTF flag tests, past
// Salvage's injection at 0x00458F99. ESI holds the item and 0x004590F6 is the function's epilogue.
// A ghosted copy skips its normal draw; the room pass draws the ghost instead.
CodeInjection item_render_skip_ghosted_patch{
    0x00458FA2,
    [](auto& regs) {
        rf::Item* item = regs.esi;
        if (per_player_respawn_is_ghosted(item)) {
            regs.eip = 0x004590F6;
        }
    },
};

void per_player_respawn_apply_patch()
{
    item_render_skip_ghosted_patch.install();
}
