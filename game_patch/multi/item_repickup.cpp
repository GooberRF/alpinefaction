#include <algorithm>
#include <map>
#include <common/utils/list-utils.h>
#include <patch_common/CodeInjection.h>
#include "item_repickup.h"
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

    int repickup_cooldown_ms(const rf::Item* item)
    {
        const int timeout_ms = g_alpine_server_config_active_rules.repickup_timeout_ms;
        return std::min(timeout_ms > 0 ? timeout_ms : item->respawn_time_ms, item_repickup_max_timeout_ms);
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

bool item_repickup_applies(const rf::Item* item)
{
    if (!rf::is_server || !g_alpine_server_config_active_rules.allow_repickup
        || !(rf::netgame.flags & rf::NG_FLAG_WEAPON_STAY) || !item || !item->info) {
        return false;
    }
    const int weapon_type = item->info->gives_weapon_id;
    return weapon_type >= 0 && !(item->item_flags & rf::IF_DROPPED) && !weapon_stay_is_exempt(weapon_type);
}

bool item_repickup_is_blocked(rf::Player* player, const rf::Item* item)
{
    auto it = player->repickup_cooldowns.find(item->handle);
    if (it == player->repickup_cooldowns.end()) {
        return false;
    }
    if (it->second.elapsed()) {
        player->repickup_cooldowns.erase(it);
        return false;
    }
    return true;
}

bool item_repickup_blocks(rf::Player* player, const rf::Item* item)
{
    if (!player || !item) {
        return false;
    }
    if (rf::is_server) {
        return item_repickup_is_blocked(player, item);
    }
    return player == rf::local_player && item_repickup_is_ghosted(item);
}

void item_repickup_on_picked_up(rf::Player* player, rf::Item* item)
{
    if (!player || !item_repickup_applies(item)) {
        return;
    }
    const int cooldown_ms = repickup_cooldown_ms(item);
    if (cooldown_ms <= 0) {
        return;
    }
    player->repickup_cooldowns[item->handle].set(cooldown_ms);
    af_send_item_repickup(player, item->handle, cooldown_ms);
}

void item_repickup_on_player_killed(rf::Player* player)
{
    if (!player) {
        return;
    }
    if (rf::is_server) {
        player->repickup_cooldowns.clear();
    }
    if (player == rf::local_player) {
        g_ghosts.clear();
    }
}

void item_repickup_level_init()
{
    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        player.repickup_cooldowns.clear();
    }
    g_ghosts.clear();
}

void item_repickup_client_on_taken(int item_handle, int remaining_ms)
{
    if (remaining_ms <= 0 || !rf::item_from_handle(item_handle)) {
        return;
    }
    ItemGhost& ghost = g_ghosts[item_handle];
    ghost.until.set(remaining_ms);
    ghost.duration_ms = remaining_ms;
}

bool item_repickup_is_ghosted(const rf::Item* item)
{
    if (g_ghosts.empty() || !item) {
        return false;
    }
    auto it = g_ghosts.find(item->handle);
    return it != g_ghosts.end() && !it->second.until.elapsed();
}

void item_repickup_render_pass(rf::GRoom* room_filter)
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
        if (item_repickup_is_ghosted(item)) {
            regs.eip = 0x004590F6;
        }
    },
};

void item_repickup_apply_patch()
{
    item_render_skip_ghosted_patch.install();
}
