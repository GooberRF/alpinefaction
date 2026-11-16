#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iterator>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
#include <common/alpine_powerup_items.h>
#include <common/gr_d3d11/gr_d3d11_mesh.h>
#include <common/utils/list-utils.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <xlog/xlog.h>
#include "powerups.h"
#include "alpine_packets.h"
#include "awards.h"
#include "bagman.h"
#include "gametype.h"
#include "haste_trails.h"
#include "jetpack.h"
#include "salvage.h"
#include "demo/demo.h"
#include "../fflink/afstats_events.h"
#include "../hud/hud.h"
#include "../hud/hud_internal.h"
#include "../rf/ai.h"
#include "../rf/bmpman.h"
#include "../rf/entity.h"
#include "../rf/item.h"
#include "../rf/multi.h"
#include "../rf/physics.h"
#include "../rf/v3d.h"
#include "../rf/vmesh.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_light.h"
#include "../rf/os/frametime.h"
#include "../rf/player/player.h"
#include "../rf/sound/sound.h"
#include "../rf/weapon.h"
#include "../misc/alpine_settings.h"
#include "../os/os.h"
#include "../sound/sound.h"

namespace
{

constexpr size_t powerup_count = std::size(alpine_powerup_item_classes);
constexpr int max_powerup_seconds = 3600;
constexpr uint32_t max_time_left_ms = max_powerup_seconds * 1000;

const std::array<rf::gr::Color, powerup_count> g_powerup_colors = [] {
    std::array<rf::gr::Color, powerup_count> colors{};
    for (size_t i = 0; i < powerup_count; ++i) {
        const uint32_t rgb = alpine_powerup_item_classes[i].color;
        colors[i] = {static_cast<rf::ubyte>(rgb >> 16), static_cast<rf::ubyte>(rgb >> 8), static_cast<rf::ubyte>(rgb)};
    }
    return colors;
}();

float g_holder_light_phase = 0.0f;
float g_holder_light_intensity = 0.0f;

struct PowerupBitmaps
{
    int hud_icon = -2; // -2 until loaded
    int overlay = -2;
};
std::array<PowerupBitmaps, powerup_count> g_powerup_bitmaps;

// `bmh` loaded from `filename` on first use, -1 when the powerup has none.
int powerup_bitmap(int& bmh, const char* filename)
{
    if (bmh == -2) {
        bmh = filename ? rf::bm::load(filename, -1, true) : -1;
    }
    return bmh;
}

int powerup_hud_icon(size_t index)
{
    return powerup_bitmap(g_powerup_bitmaps[index].hud_icon, alpine_powerup_item_classes[index].hud_icon_filename);
}

int powerup_overlay(size_t index)
{
    return powerup_bitmap(g_powerup_bitmaps[index].overlay, alpine_powerup_item_classes[index].overlay_filename);
}

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

// item_info index of each super_item_class_names entry, -1 for a class items.tbl lacks.
std::array<int, std::size(super_item_class_names)> g_super_item_types = [] {
    std::array<int, std::size(super_item_class_names)> types{};
    types.fill(-1);
    return types;
}();

// Each powerup whose item the level kept.
std::array<bool, powerup_count> g_level_powerups{};

constexpr const char* haste_jump_sound_filename = "af_haste_jump1.wav";
int g_haste_jump_sound = -1;
constexpr int64_t haste_jump_report_interval_ms = 200;
// Under the report floor, to absorb jitter between two reports.
constexpr int64_t haste_jump_relay_interval_ms = 150;

// The stock pair ahead of the Alpine powerups in the afstats registry.
constexpr int stock_powerup_count = 2;
static_assert(
    static_cast<int>(afstats::Powerup::jetpack) == stock_powerup_count + static_cast<int>(AlpinePowerup::jetpack));
static_assert(
    static_cast<int>(afstats::Powerup::haste) == stock_powerup_count + static_cast<int>(AlpinePowerup::haste));
static_assert(
    static_cast<int>(afstats::Powerup::vampire) == stock_powerup_count + static_cast<int>(AlpinePowerup::vampire));

afstats::Powerup stats_powerup(AlpinePowerup powerup)
{
    return static_cast<afstats::Powerup>(stock_powerup_count + static_cast<int>(powerup));
}

// A continuous-fire loop keeps to the hasted fire rate.
void haste_on_powerup_changed(int entity_handle, bool granted)
{
    if (const rf::Entity* ep = rf::entity_from_handle(entity_handle)) {
        snd_instance_set_speed(ep->weapon_loop_sound_handle, granted ? haste_speed_factor : 1.0f);
    }
}

void on_powerup_changed(int entity_handle, AlpinePowerup powerup, bool granted)
{
    switch (powerup) {
        case AlpinePowerup::jetpack:
            jetpack_on_powerup_changed(entity_handle, granted);
            break;
        case AlpinePowerup::haste:
            haste_on_powerup_changed(entity_handle, granted);
            break;
        case AlpinePowerup::vampire:
        case AlpinePowerup::health_shard:
        case AlpinePowerup::armor_shard:
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
        rf::Player* player = rf::player_from_entity_handle(entity_handle);
        if (!player || !player->net_data || (player->is_bot && item_type_is_bot_unusable_powerup(item->info_index))) {
            return -1;
        }
        const auto time_left_ms = static_cast<uint32_t>(std::clamp(count, 1, max_powerup_seconds)) * 1000u;
        const uint8_t player_id = player->net_data->player_id;
        const bool was_held = powerup_is_active(ep, *powerup);
        set_time_left(entity_handle, player_id, *powerup, time_left_ms / 1000.0f);
        af_send_powerup_state(nullptr, player_id, static_cast<uint8_t>(*powerup), time_left_ms);
        powerups_on_timed_pickup(player, stats_powerup(*powerup), was_held, time_left_ms);
    }

    rf::item_show_pickup_message(ep, item->info, count, false);
    return 0;
}

int __cdecl shard_item_touch(int entity_handle, int item_handle, int count)
{
    rf::Entity* ep = rf::entity_from_handle(entity_handle);
    rf::Item* item = rf::item_from_handle(item_handle);
    const auto powerup = item ? powerup_for_item_type(item->info_index) : std::nullopt;
    if (!ep || !powerup) {
        return -1;
    }

    // As with Multi Super Health and Armor (0x00480050), clients take the new value from the server.
    if (!rf::is_multi || rf::is_server) {
        // The level Multi Super Health and Armor set.
        constexpr float max_value = 200.0f;
        const bool health = alpine_powerup_item_classes[static_cast<size_t>(*powerup)].effect
            == AlpinePowerupEffect::health;
        float& value = health ? ep->life : ep->armor;
        if (value >= max_value) {
            return -1;
        }
        value = std::min(value + static_cast<float>(std::max(count, 1)), max_value);
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
            info.hud_msg_name = &def.hud_msg_name;
            info.pickup_msg_single = &def.pickup_msg;
            info.pickup_msg_multi = nullptr;
            info.pickup_msg_wa_single = nullptr;
            info.pickup_msg_wa_multi = nullptr;
            info.v3d_type = rf::MESH_TYPE_STATIC;
            // Stock damage_pickup.wav's range and volume, for a file the sounds table has not registered already.
            info.pickup_snd = def.pickup_sound_filename
                ? rf::snd_get_handle(def.pickup_sound_filename, 10.0f, 0.9f, 1.0f)
                : -1;
            info.count = def.count;
            info.count_single = def.count;
            info.count_multi = def.count;
            info.respawn_time_millis = def.respawn_time_s * 1000;
            info.ammo_for_weapon_id = -1;
            info.gives_weapon_id = -1;
            info.flags = rf::IIF_SPINS_IN_MULTI;
        }
        rf::item_info[type].touch_callback =
            def.effect == AlpinePowerupEffect::timed ? powerup_item_touch : shard_item_touch;
        g_powerup_item_types[i] = type;
    }
}

FunHook<void()> items_tbl_parse_hook{
    0x00458A50,
    []() {
        items_tbl_parse_hook.call_target();
        append_alpine_powerup_item_classes();
        // Registered at game init, as the stock pickup sounds are, so the handles outlive every level.
        g_haste_jump_sound = rf::snd_get_handle(haste_jump_sound_filename, 10.0f, 1.0f, 1.0f);
        for (size_t i = 0; i < std::size(super_item_class_names); ++i) {
            g_super_item_types[i] = rf::item_lookup_type(super_item_class_names[i]);
        }
    },
};

// The stock powerups' touch callback, replacing its `call multi_powerup_add`, a relative call no trampoline can carry.
// The arguments are on the stack, and the caller pops them.
CodeInjection item_touch_multi_powerup_add_injection{
    0x0045AB35,
    [](auto& regs) {
        rf::Player* player = addr_as_ref<rf::Player*>(regs.esp);
        const int type = addr_as_ref<int>(regs.esp + 4);
        const int time_ms = addr_as_ref<int>(regs.esp + 8);
        // The Bagman carrier's amp has no effect in Bagman modes, so it is no powerup there.
        const bool reported = type == rf::MULTI_POWERUP_INVULNERABILITY
            || (type == rf::MULTI_POWERUP_DAMAGE_AMP && !gt_is_bagman_any());
        const bool was_held = reported && rf::multi_powerup_has_player(player, type);
        rf::multi_powerup_add(player, type, time_ms);
        if (reported) {
            // multi_powerup_add sets the timer, so the time left is the item's.
            powerups_on_timed_pickup(player,
                type == rf::MULTI_POWERUP_DAMAGE_AMP ? afstats::Powerup::damage_amp : afstats::Powerup::invulnerability,
                was_held, static_cast<uint32_t>(std::max(time_ms, 0)));
        }
        regs.eip = 0x0045AB3A;
    },
    false,
};

// Colour of the light around a holder of `ep`'s first coloured powerup, null when it holds none.
const rf::gr::Color* holder_light_color(const rf::Entity* ep)
{
    for (size_t i = 0; i < powerup_count; ++i) {
        if (alpine_powerup_item_classes[i].color && powerup_is_active(ep, static_cast<AlpinePowerup>(i))) {
            return &g_powerup_colors[i];
        }
    }
    return nullptr;
}

// Replaces `call multi_ctf_get_blue_flag_player`, a relative call no trampoline can carry. Flag carriers keep the
// flag light.
CodeInjection entity_process_powerup_light_injection{
    0x0041ED06,
    [](auto& regs) {
        rf::Entity* ep = regs.esi;
        rf::Player* blue_flag_player = rf::multi_ctf_get_blue_flag_player();
        const rf::gr::Color* color = g_holder_light_intensity >= 0.0f ? holder_light_color(ep) : nullptr;
        const rf::Player* player = color ? rf::player_from_entity_handle(ep->handle) : nullptr;
        if (!player || player == blue_flag_player || player == rf::multi_ctf_get_red_flag_player()
            || salvage_player_is_carrier(player)) {
            regs.eax = blue_flag_player;
            regs.eip = 0x0041ED0B;
            return;
        }

        ep->powerup_light_handle = rf::gr::light_create_point(&ep->pos, rf::gr::powerup_light_radius,
            g_holder_light_intensity, color->red / 255.0f, color->green / 255.0f, color->blue / 255.0f, true,
            rf::gr::SHADOWCAST_EDITOR, 0);
        regs.eip = 0x0041EE2A;
    },
    false,
};

// Replaces the `call vmesh_render` at a hooked call site, a relative call no trampoline can carry; its arguments are on
// the stack. Tints the Bagman bag aura among the stock Damage Amp and Invulnerability overlays, then fills the slots they
// left free, tinting ours with their powerup's colour.
void vmesh_render_with_powerup_overlays(const rf::Entity* ep, BaseCodeInjection::Regs& regs, uintptr_t return_addr)
{
    auto* const vmesh = addr_as_ref<rf::VMesh*>(regs.esp);
    auto* const pos = addr_as_ref<rf::Vector3*>(regs.esp + 4);
    auto* const orient = addr_as_ref<rf::Matrix3*>(regs.esp + 8);
    auto* const params = addr_as_ref<rf::MeshRenderParams*>(regs.esp + 12);
    gr::d3d11::ScopedPowerupOverlayTints::Tints tints{};
    size_t slot = 0;
    for (; slot < std::size(params->powerup_bitmaps) && params->powerup_bitmaps[slot] != -1; ++slot) {
        tints[slot] = bagman_aura_tint(params->powerup_bitmaps[slot]);
    }
    for (size_t i = 0; i < powerup_count && slot < std::size(params->powerup_bitmaps); ++i) {
        if (!powerup_is_active(ep, static_cast<AlpinePowerup>(i))) {
            continue;
        }
        if (const int bmh = powerup_overlay(i); bmh >= 0) {
            if (alpine_powerup_item_classes[i].color) {
                tints[slot] = &g_powerup_colors[i];
            }
            params->powerup_bitmaps[slot++] = bmh;
        }
    }
    gr::d3d11::ScopedPowerupOverlayTints scope{tints};
    rf::vmesh_render(vmesh, pos, orient, params);
    // The caller pops the arguments after the call.
    regs.eip = return_addr;
}

// entity_render's draw of the entity's own mesh. ESI is the entity.
CodeInjection entity_render_powerup_overlay_injection{
    0x00421C03,
    [](auto& regs) {
        rf::Entity* ep = regs.esi;
        vmesh_render_with_powerup_overlays(ep, regs, 0x00421C08);
    },
    false,
};

// player_fpgun_render's draw of the weapon mesh. EBP is the player, the spectated one in spectate mode.
CodeInjection player_fpgun_render_powerup_overlay_injection{
    0x004ABBC8,
    [](auto& regs) {
        rf::Player* player = regs.ebp;
        vmesh_render_with_powerup_overlays(rf::entity_from_handle(player->entity_handle), regs, 0x004ABBCD);
    },
    false,
};

void haste_shorten(rf::Timestamp& timestamp)
{
    if (timestamp.valid() && !timestamp.elapsed()) {
        timestamp.set(static_cast<int>(timestamp.time_until() / haste_speed_factor));
    }
}

// EntityInfo is shared by the entity's whole class, so its limits are scaled only around the hasted entity's step.
FunHook<void(rf::Entity*)> physics_simulate_entity_haste_hook{
    0x0049F3C0,
    [](rf::Entity* ep) {
        if (!ep->info || !powerup_is_active(ep, AlpinePowerup::haste)) {
            physics_simulate_entity_haste_hook.call_target(ep);
            return;
        }
        rf::EntityInfo& info = *ep->info;
        const float max_vel = ep->max_vel;
        const float custom_max_vel = ep->custom_max_vel;
        const float info_max_vel = info.max_vel;
        const float acceleration = info.acceleration;
        const float ground_acceleration = rf::mp_ground_acceleration;
        ep->max_vel = max_vel * haste_speed_factor;
        ep->custom_max_vel = custom_max_vel * haste_speed_factor;
        info.max_vel = info_max_vel * haste_speed_factor;
        info.acceleration = acceleration * haste_speed_factor;
        rf::mp_ground_acceleration = ground_acceleration * haste_speed_factor;

        physics_simulate_entity_haste_hook.call_target(ep);

        // A crouch or jump pad during the step rewrites its own field, and that value stands.
        if (ep->max_vel == max_vel * haste_speed_factor) {
            ep->max_vel = max_vel;
        }
        if (ep->custom_max_vel == custom_max_vel * haste_speed_factor) {
            ep->custom_max_vel = custom_max_vel;
        }
        info.max_vel = info_max_vel;
        info.acceleration = acceleration;
        rf::mp_ground_acceleration = ground_acceleration;
    },
};

FunHook<void(rf::Entity*)> entity_jump_haste_hook{
    0x004288B0,
    [](rf::Entity* ep) {
        if (!powerup_is_active(ep, AlpinePowerup::haste)) {
            entity_jump_haste_hook.call_target(ep);
            return;
        }
        // Jump height goes with the square of the launch speed.
        const float jump_velocity = rf::jump_velocity;
        rf::jump_velocity = jump_velocity * std::sqrt(haste_speed_factor);
        const float vel_y = ep->p_data.vel.y;
        entity_jump_haste_hook.call_target(ep);
        rf::jump_velocity = jump_velocity;
        // The stock jump sets the vertical velocity only when it jumps, and plays its sound 2D, for the jumper alone.
        // Everyone else hears it in 3D through the server.
        if (ep->p_data.vel.y == vel_y) {
            return;
        }
        if (ep == rf::local_player_entity && g_haste_jump_sound >= 0) {
            play_local_sound_2d(static_cast<uint16_t>(g_haste_jump_sound), 0, 1.0f);
        }
        if (rf::is_server) {
            powerups_server_on_haste_jump(rf::player_from_entity_handle(ep->handle));
        }
        else if (ep == rf::local_player_entity && rf::local_player) {
            // A swimmer at the surface can jump every frame.
            std::optional<int64_t>& reported_ms = rf::local_player->last_haste_jump_ms;
            const int64_t now = timer::get_i64(1000);
            if (!reported_ms || now - *reported_ms >= haste_jump_report_interval_ms) {
                reported_ms = now;
                af_send_haste_jump_request();
            }
        }
    },
};

// player_fire_primary_weapon: the local player's fire wait. ESI is the firing entity, the hull when driving.
CodeInjection player_fire_primary_weapon_haste_injection{
    0x004A515F,
    [](auto& regs) {
        rf::Entity* ep = regs.esi;
        regs.eax = haste_scale_ms(ep, regs.eax);
    },
};

// player_fire_primary_weapon, once a throw or swing has its release delayed by create_weapon_delay: the animation
// it waits on runs faster under Haste. EBP is the entity.
CodeInjection player_fire_primary_weapon_release_haste_injection{
    0x004A57FA,
    [](auto& regs) {
        rf::Entity* ep = regs.ebp;
        if (powerup_is_active(ep, AlpinePowerup::haste)) {
            for (rf::Timestamp& timestamp : ep->ai.create_weapon_delay_timestamps) {
                haste_shorten(timestamp);
            }
        }
    },
};

// entity_turn_weapon_on: the riot stick taser's fire wait. ESI is the entity.
CodeInjection entity_turn_weapon_on_haste_injection{
    0x0041ABE4,
    [](auto& regs) {
        rf::Entity* ep = regs.esi;
        regs.eax = haste_scale_ms(ep, regs.eax);
    },
};

// entity_turn_weapon_on, storing the continuous-fire loop it just started. ESI is the entity, EAX the instance.
CodeInjection entity_turn_weapon_on_loop_sound_haste_injection{
    0x0041AA95,
    [](auto& regs) {
        rf::Entity* ep = regs.esi;
        if (powerup_is_active(ep, AlpinePowerup::haste)) {
            snd_instance_set_speed(regs.eax, haste_speed_factor);
        }
    },
};

FunHook<void(rf::AiInfo*)> ai_fire_bursts_haste_hook{
    0x00409280,
    [](rf::AiInfo* ai) {
        const int primary_left = ai->primary_burst_fire_remaining;
        const int secondary_left = ai->secondary_burst_fire_remaining;
        ai_fire_bursts_haste_hook.call_target(ai);
        if (!powerup_is_active(ai->ep, AlpinePowerup::haste)) {
            return;
        }
        if (ai->primary_burst_fire_remaining < primary_left) {
            haste_shorten(ai->primary_burst_fire_next_timestamp);
        }
        if (ai->secondary_burst_fire_remaining < secondary_left) {
            haste_shorten(ai->secondary_burst_fire_next_timestamp);
        }
    },
};

// Runs on the server and, from the server's reload packet, on the reloading client.
FunHook<bool __cdecl(rf::Entity*, bool, bool)> entity_reload_current_primary_haste_hook{
    0x00425280,
    [](rf::Entity* ep, bool no_sound, bool is_reload_packet) {
        const bool reloaded = entity_reload_current_primary_haste_hook.call_target(ep, no_sound, is_reload_packet);
        if (reloaded && powerup_is_active(ep, AlpinePowerup::haste)) {
            haste_shorten(ep->reload_done_timestamp);
            haste_shorten(ep->drop_clip_timestamp);
            haste_shorten(ep->zero_ammo_timestamp);
        }
        return reloaded;
    },
};

// Whether the player `is_owner` picks out holds Haste: hooks that see only a mesh or a timer can't assume the local
// player, as player_fpgun_process also runs for the spectated one.
bool owner_is_hasted(const auto& is_owner)
{
    if (!powerup_any_active(AlpinePowerup::haste)) {
        return false;
    }
    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (is_owner(player)) {
            return powerup_is_active(rf::entity_from_handle(player.entity_handle), AlpinePowerup::haste);
        }
    }
    return false;
}

// player_fpgun_process: the pause before the next weapon is drawn.
CallHook<int __fastcall(rf::Timestamp*, int, int)> player_fpgun_draw_delay_haste_hook{
    0x004AAD67,
    [](rf::Timestamp* timestamp, int edx, int delay_ms) FASTCALL_LAMBDA {
        if (owner_is_hasted(
                [timestamp](rf::Player& player) { return &player.fpgun_data.draw_delay_timestamp == timestamp; })) {
            delay_ms = static_cast<int>(delay_ms / haste_speed_factor);
        }
        return player_fpgun_draw_delay_haste_hook.call_target(timestamp, edx, delay_ms);
    },
};

// player_fpgun_process: the first-person weapon animations, which also gate firing, reloading and switching.
CallHook<void(rf::VMesh*, float, int, rf::Vector3*, rf::Matrix3*, int)> player_fpgun_process_vmesh_haste_hook{
    0x004AACC7,
    [](rf::VMesh* mesh, float frametime, int increment_only, rf::Vector3* pos, rf::Matrix3* orient, int lod_level) {
        if (owner_is_hasted([mesh](rf::Player& player) { return player.weapon_mesh_handle == mesh; })) {
            frametime *= haste_speed_factor;
        }
        player_fpgun_process_vmesh_haste_hook.call_target(mesh, frametime, increment_only, pos, orient, lod_level);
    },
};

// entity_process: the third-person character animations everyone sees.
CallHook<void(rf::VMesh*, float, int, rf::Vector3*, rf::Matrix3*, int)> entity_process_vmesh_haste_hook{
    0x0041DD41,
    [](rf::VMesh* mesh, float frametime, int increment_only, rf::Vector3* pos, rf::Matrix3* orient, int lod_level) {
        if (owner_is_hasted([mesh](rf::Player& player) {
                const rf::Entity* ep = rf::entity_from_handle(player.entity_handle);
                return ep && ep->vmesh == mesh;
            })) {
            frametime *= haste_speed_factor;
        }
        entity_process_vmesh_haste_hook.call_target(mesh, frametime, increment_only, pos, orient, lod_level);
    },
};

} // namespace

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

bool powerup_is_active(const rf::Entity* ep, AlpinePowerup powerup)
{
    return powerup_time_left(ep, powerup) > 0.0f;
}

float powerup_time_left(const rf::Entity* ep, AlpinePowerup powerup)
{
    // Only multiplayer level init clears the map, so a session that ended mid-powerup leaves it stale.
    if (!ep || !rf::is_multi || g_powerups.empty()) {
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

size_t super_item_priority(int item_type)
{
    if (item_type < 0) {
        return g_super_item_types.size();
    }
    return std::ranges::find(g_super_item_types, item_type) - g_super_item_types.begin();
}

bool item_type_is_super(int item_type)
{
    return super_item_priority(item_type) < g_super_item_types.size();
}

int powerup_item_type(AlpinePowerup powerup)
{
    return g_powerup_item_types[static_cast<size_t>(powerup)];
}

const rf::gr::Color& powerup_color(AlpinePowerup powerup)
{
    return g_powerup_colors[static_cast<size_t>(powerup)];
}

bool item_type_is_bot_unusable_powerup(int item_type)
{
    return powerup_for_item_type(item_type) == AlpinePowerup::jetpack;
}

bool item_type_is_alpine_shard(int item_type)
{
    const auto powerup = powerup_for_item_type(item_type);
    return powerup && alpine_powerup_item_classes[static_cast<size_t>(*powerup)].effect != AlpinePowerupEffect::timed;
}

int haste_scale_ms(const rf::Entity* ep, int ms)
{
    return powerup_is_active(ep, AlpinePowerup::haste) ? static_cast<int>(ms / haste_speed_factor) : ms;
}

void powerup_append_hud_icons(const rf::Entity* ep, std::vector<int>& icons)
{
    for (size_t i = 0; i < powerup_count; ++i) {
        if (!powerup_is_active(ep, static_cast<AlpinePowerup>(i))) {
            continue;
        }
        if (const int bmh = powerup_hud_icon(i); bmh >= 0) {
            icons.push_back(bmh);
        }
    }
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
    g_level_powerups[static_cast<size_t>(*powerup)] = true;
    return item_type;
}

bool level_has_alpine_powerups()
{
    return std::ranges::any_of(g_level_powerups, std::identity{});
}

bool level_has_powerup(AlpinePowerup powerup)
{
    // Bagman deletes the placed super items after they load.
    if (gt_is_bagman_any() && item_type_is_super(powerup_item_type(powerup))) {
        return false;
    }
    return g_level_powerups[static_cast<size_t>(powerup)];
}

void powerups_on_timed_pickup(rf::Player* player, afstats::Powerup powerup, bool was_held, uint32_t time_left_ms)
{
    if (!rf::is_multi || !rf::is_server) {
        return;
    }
    afstats::on_powerup_pickup(player, powerup, was_held, time_left_ms);
    awards_on_powerup_pickup(player, powerup);
}

bool powerup_stats_held(rf::Player* player, afstats::Powerup powerup)
{
    switch (powerup) {
        case afstats::Powerup::damage_amp:
            // Bagman modes patch out its damage, so holding it there is no powerup.
            return !gt_is_bagman_any() && rf::multi_powerup_has_player(player, rf::MULTI_POWERUP_DAMAGE_AMP);
        case afstats::Powerup::invulnerability:
            return rf::multi_powerup_has_player(player, rf::MULTI_POWERUP_INVULNERABILITY);
        case afstats::Powerup::jetpack:
        case afstats::Powerup::haste:
        case afstats::Powerup::vampire:
            break;
    }
    return powerup_is_active(rf::entity_from_handle(player->entity_handle),
        static_cast<AlpinePowerup>(static_cast<int>(powerup) - stock_powerup_count));
}

uint8_t powerup_stats_mask(rf::Player* player)
{
    uint8_t mask = 0;
    for (size_t i = 0; i < afstats::powerup_count; ++i) {
        if (powerup_stats_held(player, static_cast<afstats::Powerup>(i))) {
            mask |= static_cast<uint8_t>(1u << i);
        }
    }
    return mask;
}

void powerups_apply_remote_state(uint8_t player_id, uint8_t powerup_id, uint32_t time_left_ms)
{
    if (powerup_id >= powerup_count || alpine_powerup_item_classes[powerup_id].effect != AlpinePowerupEffect::timed) {
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

void powerups_server_on_haste_jump(rf::Player* jumper)
{
    if (!rf::is_server || !jumper || !jumper->net_data) {
        return;
    }
    rf::Entity* ep = rf::entity_from_handle(jumper->entity_handle);
    if (!ep || rf::entity_is_dying(ep) || !powerup_is_active(ep, AlpinePowerup::haste)) {
        return;
    }
    const int64_t now = timer::get_i64(1000);
    if (jumper->last_haste_jump_ms && now - *jumper->last_haste_jump_ms < haste_jump_relay_interval_ms) {
        return;
    }
    jumper->last_haste_jump_ms.emplace(now);
    af_send_haste_jump(jumper->net_data->player_id);
    if (jumper != rf::local_player) {
        powerups_on_remote_haste_jump(jumper->net_data->player_id);
    }
}

void powerups_on_remote_haste_jump(uint8_t player_id)
{
    rf::Player* player = rf::multi_find_player_by_id(player_id);
    rf::Entity* ep = player ? rf::entity_from_handle(player->entity_handle) : nullptr;
    if (rf::is_dedicated_server || g_haste_jump_sound < 0 || !ep || ep == rf::local_player_entity
        || rf::entity_is_dying(ep)) {
        return;
    }
    rf::snd_play_3d(g_haste_jump_sound, ep->pos, 1.0f, rf::Vector3{}, rf::SOUND_GROUP_EFFECTS);
}

void powerups_level_init()
{
    g_powerups.clear();
    g_level_powerups.fill(false);
    haste_trails_clear();
}

void powerups_level_init_post()
{
    if (powerup_item_type(AlpinePowerup::haste) < 0) {
        return;
    }
    for (int type = 0; type < rf::num_weapon_types; ++type) {
        const rf::WeaponInfo& info = rf::weapon_types[type];
        // Players also fire the Machine Pistol's alternate mode, which is a weapon of its own.
        if (!(info.flags & rf::WTF_PLAYER_WEP) && type != rf::machine_pistol_special_weapon_type) {
            continue;
        }
        const std::pair<int, int> loops[] = {
            {rf::WTF_CONTINUOUS_FIRE, info.launch_foley_id},
            {rf::WTF_ALT_CONTINUOUS_FIRE, info.alt_launch_foley_id},
        };
        for (const auto& [flag, foley_id] : loops) {
            if (!(info.flags & flag)) {
                continue;
            }
            int handle;
            for (int i = 0; (handle = rf::foley_get_sound_handle_at(foley_id, i)) >= 0; ++i) {
                if (handle < rf::g_num_sounds && rf::sounds[handle].is_looping) {
                    snd_prepare_looping_speed(handle, haste_speed_factor);
                }
            }
        }
    }
}

void powerups_do_frame()
{
    haste_trails_do_frame();
    if (!rf::is_multi || g_powerups.empty()) {
        return;
    }

    // Runs from rf_do_frame, outside the sim frame the demo pause freezes.
    const float dt = rf::frametime * demo_playback_sim_time_scale();
    g_holder_light_intensity = salvage_advance_pickup_light_pulse(g_holder_light_phase);
    std::vector<std::tuple<int, AlpinePowerup, bool>> expired;
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
                expired.emplace_back(entity_handle, static_cast<AlpinePowerup>(i), alive);
            }
        }
    }

    for (const auto& [entity_handle, powerup, alive] : expired) {
        const auto it = g_powerups.find(entity_handle);
        if (it == g_powerups.end()) {
            continue;
        }
        const uint8_t player_id = it->second.player_id;
        set_time_left(entity_handle, player_id, powerup, 0.0f);
        // A holder who left is no longer found; their holds ended as they left.
        if (rf::Player* player = rf::is_server ? rf::multi_find_player_by_id(player_id) : nullptr) {
            af_send_powerup_state(nullptr, player_id, static_cast<uint8_t>(powerup), 0);
            afstats::on_powerup_end(player, stats_powerup(powerup),
                alive ? afstats::PowerupEndReason::expired : afstats::PowerupEndReason::died);
        }
    }
}

// The local player's icons extend the stock powerup icon row outward, alternating sides in table order.
void powerups_render_hud()
{
    const rf::Entity* ep = rf::local_player_entity;
    if (!ep || g_powerups.empty() || is_hud_effectively_hidden() || !hud_element_visible(HudElement::powerups)) {
        return;
    }
    const float scale = g_alpine_game_config.big_hud ? 2.0f : 1.0f;
    const int clip_w = rf::gr::clip_width();
    const int clip_h = rf::gr::clip_height();
    // The stock row's layout on the 640-wide HUD.
    constexpr int row_bottom_offset = 53;
    constexpr int invuln_icon_x = 271;
    constexpr int amp_icon_x = 324;
    constexpr int amp_icon_w = 38;
    constexpr int hud_center_x = 320;
    constexpr int gap = 3;
    const int y = hud_scale_value(clip_h - row_bottom_offset, clip_h, scale);
    int left_x = invuln_icon_x - gap;
    int right_x = amp_icon_x + amp_icon_w + gap;
    bool next_left = true;
    for (size_t i = 0; i < powerup_count; ++i) {
        const int bmh = powerup_hud_icon(i);
        if (bmh < 0) {
            continue;
        }
        int w = 0, h = 0;
        rf::bm::get_dimensions(bmh, &w, &h);
        int icon_x;
        if (next_left) {
            left_x -= w;
            icon_x = left_x;
            left_x -= gap;
        }
        else {
            icon_x = right_x;
            right_x += w + gap;
        }
        next_left = !next_left;
        if (!powerup_is_active(ep, static_cast<AlpinePowerup>(i))) {
            continue;
        }
        // Kept centred, as the stock row is: hud_transform_value would anchor an icon past 426 to the right edge.
        const int x = clip_w / 2 + static_cast<int>(std::round((icon_x - hud_center_x) * scale));
        rf::gr::set_color(255, 255, 255, 255);
        hud_scaled_bitmap(bmh, x, y, scale, rf::gr::bitmap_wrap_mode);
    }
}

void powerups_do_patch()
{
    items_tbl_parse_hook.install();
    item_touch_multi_powerup_add_injection.install();
    entity_process_powerup_light_injection.install();
    entity_render_powerup_overlay_injection.install();
    player_fpgun_render_powerup_overlay_injection.install();
    physics_simulate_entity_haste_hook.install();
    entity_jump_haste_hook.install();
    player_fire_primary_weapon_haste_injection.install();
    player_fire_primary_weapon_release_haste_injection.install();
    entity_turn_weapon_on_haste_injection.install();
    entity_turn_weapon_on_loop_sound_haste_injection.install();
    ai_fire_bursts_haste_hook.install();
    entity_reload_current_primary_haste_hook.install();
    player_fpgun_draw_delay_haste_hook.install();
    player_fpgun_process_vmesh_haste_hook.install();
    entity_process_vmesh_haste_hook.install();
}
