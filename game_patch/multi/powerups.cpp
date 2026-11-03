#include <algorithm>
#include <array>
#include <iterator>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>
#include <common/alpine_powerup_items.h>
#include <patch_common/FunHook.h>
#include <xlog/xlog.h>
#include "powerups.h"
#include "alpine_packets.h"
#include "jetpack.h"
#include "demo/demo.h"
#include "../rf/entity.h"
#include "../rf/item.h"
#include "../rf/multi.h"
#include "../rf/vmesh.h"
#include "../rf/os/frametime.h"
#include "../rf/player/player.h"

namespace
{

constexpr size_t powerup_count = std::size(alpine_powerup_item_classes);
constexpr int max_powerup_seconds = 3600;
constexpr uint32_t max_time_left_ms = max_powerup_seconds * 1000;

struct EntityPowerups
{
    uint8_t player_id = 0;
    std::array<float, powerup_count> time_left{};
};

// Keyed by local entity handle, so a powerup ends with the life that picked it up.
std::unordered_map<int, EntityPowerups> g_powerups;

// item_info index of each alpine_powerup_item_classes entry, -1 when items.tbl left no room for it.
std::array<int, powerup_count> g_powerup_item_types = [] {
    std::array<int, powerup_count> types{};
    types.fill(-1);
    return types;
}();

bool g_level_has_powerups = false;

std::optional<AlpinePowerup> powerup_for_item_type(int item_type)
{
    if (item_type < 0) {
        return std::nullopt;
    }
    const auto it = std::ranges::find(g_powerup_item_types, item_type);
    if (it == g_powerup_item_types.end()) {
        return std::nullopt;
    }
    return static_cast<AlpinePowerup>(std::distance(g_powerup_item_types.begin(), it));
}

void on_powerup_changed(int entity_handle, AlpinePowerup powerup, bool granted)
{
    switch (powerup) {
        case AlpinePowerup::jetpack:
            jetpack_on_powerup_changed(entity_handle, granted);
            break;
    }
}

void set_time_left(int entity_handle, uint8_t player_id, AlpinePowerup powerup, float time_left)
{
    const auto index = static_cast<size_t>(powerup);
    if (time_left > 0.0f) {
        auto& state = g_powerups[entity_handle];
        state.player_id = player_id;
        state.time_left[index] = time_left;
        on_powerup_changed(entity_handle, powerup, true);
        return;
    }

    const auto it = g_powerups.find(entity_handle);
    if (it == g_powerups.end() || it->second.time_left[index] <= 0.0f) {
        return;
    }
    it->second.time_left[index] = 0.0f;
    if (std::ranges::all_of(it->second.time_left, [](float t) { return t <= 0.0f; })) {
        g_powerups.erase(it);
    }
    on_powerup_changed(entity_handle, powerup, false);
}

int __cdecl powerup_item_touch(int entity_handle, int item_handle, int count)
{
    rf::Entity* ep = rf::entity_from_handle(entity_handle);
    rf::Item* item = rf::item_from_handle(item_handle);
    const auto powerup = item ? powerup_for_item_type(item->info_index) : std::nullopt;
    if (!rf::is_multi || !ep || !powerup) {
        return -1;
    }

    // Clients only reach this through the server's item_apply, so they must always accept.
    if (rf::is_server) {
        // Bots cannot use powerups, so the pickup is left for players.
        rf::Player* player = rf::player_from_entity_handle(entity_handle);
        if (!player || !player->net_data || player->is_bot) {
            return -1;
        }
        const auto time_left_ms = static_cast<uint32_t>(std::clamp(count, 1, max_powerup_seconds)) * 1000u;
        const uint8_t player_id = player->net_data->player_id;
        set_time_left(entity_handle, player_id, *powerup, time_left_ms / 1000.0f);
        af_send_powerup_state(nullptr, player_id, static_cast<uint8_t>(*powerup), time_left_ms);
    }

    rf::item_show_pickup_message(ep, item->info, count, false);
    return 0;
}

void append_alpine_powerup_item_classes()
{
    for (size_t i = 0; i < powerup_count; ++i) {
        const AlpinePowerupItemClass& def = alpine_powerup_item_classes[i];
        int type = rf::item_lookup_type(def.class_name);
        if (type < 0) {
            if (rf::num_item_types >= static_cast<int>(std::size(rf::item_info))) {
                xlog::warn("[Powerups] items.tbl leaves no room for '{}'", def.class_name);
                continue;
            }
            type = rf::num_item_types++;
            rf::ItemInfo& info = rf::item_info[type];
            info.cls_name = def.class_name;
            info.v3d_filename = def.v3d_filename;
            info.hud_msg_name = reinterpret_cast<const char*>(&def.hud_msg_name);
            info.pickup_msg_single = &def.pickup_msg;
            info.pickup_msg_multi = nullptr;
            info.pickup_msg_wa_single = nullptr;
            info.pickup_msg_wa_multi = nullptr;
            info.v3d_type = rf::MESH_TYPE_STATIC;
            info.pickup_snd = -1;
            info.count = def.count;
            info.count_single = def.count;
            info.count_multi = def.count;
            info.respawn_time_millis = def.respawn_time_s * 1000;
            info.ammo_for_weapon_id = -1;
            info.gives_weapon_id = -1;
            info.flags = rf::IIF_SPINS_IN_MULTI;
        }
        rf::item_info[type].touch_callback = powerup_item_touch;
        g_powerup_item_types[i] = type;
    }
}

FunHook<void()> items_tbl_parse_hook{
    0x00458A50,
    []() {
        items_tbl_parse_hook.call_target();
        append_alpine_powerup_item_classes();
    },
};

} // namespace

bool powerup_is_active(const rf::Entity* ep, AlpinePowerup powerup)
{
    return powerup_time_left(ep, powerup) > 0.0f;
}

float powerup_time_left(const rf::Entity* ep, AlpinePowerup powerup)
{
    // Only multiplayer level init clears the map, so a session that ended mid-powerup leaves it stale.
    if (!ep || !rf::is_multi) {
        return 0.0f;
    }
    const auto it = g_powerups.find(ep->handle);
    return it != g_powerups.end() ? it->second.time_left[static_cast<size_t>(powerup)] : 0.0f;
}

bool powerup_any_active(AlpinePowerup powerup)
{
    const auto index = static_cast<size_t>(powerup);
    return rf::is_multi && std::ranges::any_of(g_powerups,
        [index](const auto& entry) { return entry.second.time_left[index] > 0.0f; });
}

bool item_type_is_alpine_powerup(int item_type)
{
    return powerup_for_item_type(item_type).has_value();
}

int powerups_filter_level_item(int item_type)
{
    const auto powerup = powerup_for_item_type(item_type);
    if (!powerup) {
        return item_type;
    }
    if (*powerup == AlpinePowerup::jetpack && rf::is_multi && jetpacks_are_active()) {
        return -1;
    }
    g_level_has_powerups = true;
    return item_type;
}

bool level_has_alpine_powerups()
{
    return g_level_has_powerups;
}

void powerups_apply_remote_state(uint8_t player_id, uint8_t powerup_id, uint32_t time_left_ms)
{
    if (powerup_id >= powerup_count) {
        return;
    }
    const auto powerup = static_cast<AlpinePowerup>(powerup_id);

    if (time_left_ms == 0) {
        const auto index = static_cast<size_t>(powerup_id);
        const auto it = std::ranges::find_if(g_powerups, [player_id, index](const auto& entry) {
            return entry.second.player_id == player_id && entry.second.time_left[index] > 0.0f;
        });
        if (it != g_powerups.end()) {
            set_time_left(it->first, player_id, powerup, 0.0f);
        }
        return;
    }

    rf::Player* player = rf::multi_find_player_by_id(player_id);
    rf::Entity* ep = player ? rf::entity_from_handle(player->entity_handle) : nullptr;
    if (!ep || rf::entity_is_dying(ep)) {
        return;
    }
    set_time_left(ep->handle, player_id, powerup, std::min(time_left_ms, max_time_left_ms) / 1000.0f);
}

void powerups_send_state_to(rf::Player* player)
{
    for (const auto& [entity_handle, state] : g_powerups) {
        for (size_t i = 0; i < powerup_count; ++i) {
            if (state.time_left[i] > 0.0f) {
                const auto time_left_ms = std::max(static_cast<uint32_t>(state.time_left[i] * 1000.0f), 1u);
                af_send_powerup_state(player, state.player_id, static_cast<uint8_t>(i), time_left_ms);
            }
        }
    }
}

void powerups_level_init()
{
    g_powerups.clear();
    g_level_has_powerups = false;
}

void powerups_do_frame()
{
    if (!rf::is_multi || g_powerups.empty()) {
        return;
    }

    // Runs from rf_do_frame, outside the sim frame the demo pause freezes.
    const float dt = rf::frametime * demo_playback_sim_time_scale();
    std::vector<std::pair<int, AlpinePowerup>> expired;
    for (auto& [entity_handle, state] : g_powerups) {
        rf::Entity* ep = rf::entity_from_handle(entity_handle);
        rf::Player* player = rf::multi_find_player_by_id(state.player_id);
        const bool alive = ep && !rf::entity_is_dying(ep) && player && player->entity_handle == entity_handle;
        for (size_t i = 0; i < powerup_count; ++i) {
            float& time_left = state.time_left[i];
            if (time_left <= 0.0f) {
                continue;
            }
            if (alive && time_left > dt) {
                time_left -= dt;
            }
            else {
                expired.emplace_back(entity_handle, static_cast<AlpinePowerup>(i));
            }
        }
    }

    for (const auto& [entity_handle, powerup] : expired) {
        const auto it = g_powerups.find(entity_handle);
        if (it == g_powerups.end()) {
            continue;
        }
        const uint8_t player_id = it->second.player_id;
        set_time_left(entity_handle, player_id, powerup, 0.0f);
        if (rf::is_server && rf::multi_find_player_by_id(player_id)) {
            af_send_powerup_state(nullptr, player_id, static_cast<uint8_t>(powerup), 0);
        }
    }
}

void powerups_do_patch()
{
    items_tbl_parse_hook.install();
}
