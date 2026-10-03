#include <cstdint>
#include <optional>
#include <unordered_map>
#include "vehicle.h"
#include "vehicle_horn.h"
#include "vehicle_internal.h"
#include "../alpine_packets.h"
#include "../../misc/alpine_settings.h"
#include "../../os/console.h"
#include "../../rf/entity.h"
#include "../../rf/gameseq.h"
#include "../../rf/multi.h"
#include "../../rf/os/console.h"
#include "../../rf/os/timestamp.h"
#include "../../rf/player/control_config.h"
#include "../../rf/player/player.h"
#include "../../rf/sound/sound.h"
#include "../../sound/sound.h"

namespace
{
    // Server: least time between two horn STARTs one player gets to sound; a START inside it waits.
    constexpr int vehicle_horn_start_min_ms = 250;
    // Client: a horn loop that failed to start is not retried sooner than this.
    constexpr int vehicle_horn_retry_ms = 500;

    struct HornSound
    {
        int instance = -1;
        rf::Timestamp retry;
    };

    // Client: the playing horn loop of each hull, by local hull handle.
    std::unordered_map<int, HornSound> g_horn_sounds;

    bool vehicle_horn_holder_ok(rf::Entity* hull, const rf::Player* pp)
    {
        if (!hull || !pp || pp->entity_handle == -1 || !rf::entity_is_jeep(hull)) {
            return false;
        }
        rf::Entity* rider = rf::entity_from_handle(pp->entity_handle);
        return rider && !rf::entity_is_dying(rider) && vehicle_seat_leech(hull, 0) == rider->handle;
    }

    void vehicle_server_horn_broadcast(int vehicle_handle)
    {
        if (const rf::Entity* hull = vehicle_synced_entity(vehicle_handle)) {
            vehicle_broadcast_seat_occupancy(vehicle_handle, hull, -1);
        }
    }

    // A START waits out the requester's window rather than being dropped: the client reports edges only.
    void vehicle_server_horn_try_sound(int vehicle_handle, VehicleHorn& horn)
    {
        rf::Timestamp& window = g_vehicle_state.horn_cooldown[horn.requester_id];
        if (window.valid() && !window.elapsed()) {
            return;
        }
        window.set(vehicle_horn_start_min_ms);
        horn.on = true;
        vehicle_server_horn_broadcast(vehicle_handle);
    }

    // The jeep the local player drives, while he is alive in its driver seat.
    rf::Entity* vehicle_local_horn_vehicle()
    {
        rf::Entity* rider = rf::local_player_entity;
        if (!rider || rf::entity_is_dying(rider)) {
            return nullptr;
        }
        rf::Entity* hull = vehicle_ridden_live_hull(rider);
        return hull && rf::entity_is_jeep(hull) && vehicle_seat_leech(hull, 0) == rider->handle ? hull : nullptr;
    }

    void vehicle_horn_report(int vehicle_handle, bool held)
    {
        const rf::Entity* hull = rf::entity_from_handle(vehicle_handle);
        if (!hull) {
            return;
        }
        if (rf::is_server) {
            vehicle_server_handle_horn_request(rf::local_player, hull->handle, held);
        }
        else {
            af_send_vehicle_fire_request(hull->server_handle, held ? AF_VEHICLE_HORN_START : AF_VEHICLE_HORN_STOP, 0);
        }
    }

    void vehicle_horn_report_edges()
    {
        rf::Entity* hull = vehicle_local_horn_vehicle();
        bool held = false;
        if (hull && rf::local_player && !rf::game_paused) {
            held = rf::control_is_control_down(&rf::local_player->settings.controls, rf::CC_ACTION_PRIMARY_ATTACK);
        }
        const int handle = hull ? hull->handle : -1;
        if (handle != g_vehicle_state.reported_horn_vehicle) {
            if (g_vehicle_state.reported_horn_held) {
                vehicle_horn_report(g_vehicle_state.reported_horn_vehicle, false);
            }
            g_vehicle_state.reported_horn_vehicle = handle;
            g_vehicle_state.reported_horn_held = false;
        }
        if (held != g_vehicle_state.reported_horn_held) {
            vehicle_horn_report(handle, held);
            g_vehicle_state.reported_horn_held = held;
        }
    }

    // The local driver hears his own button at once; everyone else hears what the server states.
    bool vehicle_horn_audible(int vehicle_handle)
    {
        rf::Entity* hull = vehicle_live_synced_entity(vehicle_handle);
        if (!hull || !rf::entity_is_jeep(hull)) {
            return false;
        }
        const rf::Entity* driver = vehicle_driver_entity(hull);
        if (!driver) {
            return false;
        }
        if (driver == rf::local_player_entity) {
            return g_vehicle_state.reported_horn_held && g_vehicle_state.reported_horn_vehicle == vehicle_handle;
        }
        if (!g_alpine_game_config.vehicle_horns) {
            return false;
        }
        if (rf::is_server) {
            return vehicle_server_horn_sounding(vehicle_handle);
        }
        const VehicleState* st = vehicle_hull_state(vehicle_handle);
        return st && st->horn;
    }

    void vehicle_horn_stop_sound(const HornSound& sound)
    {
        if (sound.instance >= 0) {
            rf::snd_stop(sound.instance);
        }
    }

    void vehicle_horn_sustain(int vehicle_handle)
    {
        const int sound_id = get_jeep_horn_sound_id();
        if (sound_id < 0 || !vehicle_horn_audible(vehicle_handle)) {
            return;
        }
        const rf::Entity* hull = rf::entity_from_handle(vehicle_handle);
        HornSound& sound = g_horn_sounds[vehicle_handle];
        if (snd_instance_is_playing(sound.instance)) {
            rf::snd_change_3d(sound.instance, hull->pos, hull->p_data.vel, 1.0f);
            return;
        }
        if (sound.retry.valid() && !sound.retry.elapsed()) {
            return;
        }
        sound.instance = rf::snd_play_3d(sound_id, hull->pos, 1.0f, rf::zero_vector, rf::SOUND_GROUP_EFFECTS);
        if (sound.instance < 0) {
            sound.retry.set(vehicle_horn_retry_ms);
        }
    }

    void vehicle_horn_update_sounds()
    {
        std::erase_if(g_horn_sounds, [](const auto& entry) {
            if (vehicle_horn_audible(entry.first)) {
                return false;
            }
            vehicle_horn_stop_sound(entry.second);
            return true;
        });
        if (g_vehicle_state.reported_horn_held) {
            vehicle_horn_sustain(g_vehicle_state.reported_horn_vehicle);
        }
        if (rf::is_server) {
            for (const auto& [handle, horn] : g_vehicle_state.horn) {
                if (horn.on) {
                    vehicle_horn_sustain(handle);
                }
            }
        }
        else {
            for (const auto& [handle, st] : g_vehicle_state.hull_state) {
                if (st.horn) {
                    vehicle_horn_sustain(handle);
                }
            }
        }
    }

    ConsoleCommand2 vehiclehorns_cmd{
        "cl_vehiclehorns",
        [](std::optional<bool> enabled) {
            g_alpine_game_config.vehicle_horns = enabled.value_or(!g_alpine_game_config.vehicle_horns);
            rf::console::print("Other players' vehicle horns are {}",
                               g_alpine_game_config.vehicle_horns ? "audible" : "muted");
        },
        "Hear other players' jeep horns in multiplayer; your own horn always plays",
        "cl_vehiclehorns [bool]",
    };
} // namespace

void vehicle_server_handle_horn_request(rf::Player* pp, int vehicle_handle, bool held)
{
    if (!rf::is_server || !pp || !pp->net_data) {
        return;
    }
    const uint8_t player_id = pp->net_data->player_id;
    if (!held) {
        // Never rate limited, and only the player sounding a horn may silence it.
        auto it = g_vehicle_state.horn.find(vehicle_handle);
        if (it == g_vehicle_state.horn.end() || it->second.requester_id != player_id) {
            return;
        }
        const bool was_on = it->second.on;
        g_vehicle_state.horn.erase(it);
        if (was_on) {
            vehicle_server_horn_broadcast(vehicle_handle);
        }
        return;
    }
    rf::Entity* hull = vehicle_live_synced_entity(vehicle_handle);
    if (!vehicle_horn_holder_ok(hull, pp)) {
        return;
    }
    // A horn still recorded for the previous driver restarts as this driver's, so clients hear the change.
    VehicleHorn& horn = g_vehicle_state.horn[hull->handle];
    if (horn.requester_id != player_id) {
        horn.on = false;
    }
    horn.requester_id = player_id;
    if (!horn.on) {
        vehicle_server_horn_try_sound(hull->handle, horn);
    }
}

bool vehicle_server_horn_sounding(int vehicle_handle)
{
    auto it = g_vehicle_state.horn.find(vehicle_handle);
    if (it == g_vehicle_state.horn.end() || !it->second.on) {
        return false;
    }
    return vehicle_horn_holder_ok(vehicle_live_synced_entity(vehicle_handle),
                                  rf::multi_find_player_by_id(it->second.requester_id));
}

// Every stop the request path never hears about: exit, seat swap, death, disconnect, a dying or
// deleted hull. A seat change's own statement already says "off" through vehicle_server_horn_sounding.
void vehicle_server_horn_do_frame()
{
    for (auto it = g_vehicle_state.horn.begin(); it != g_vehicle_state.horn.end();) {
        const int handle = it->first;
        rf::Entity* hull = vehicle_live_synced_entity(handle);
        if (!vehicle_horn_holder_ok(hull, rf::multi_find_player_by_id(it->second.requester_id))) {
            const bool was_on = it->second.on;
            it = g_vehicle_state.horn.erase(it);
            if (was_on) {
                vehicle_server_horn_broadcast(handle);
            }
            continue;
        }
        if (!it->second.on) {
            vehicle_server_horn_try_sound(handle, it->second);
        }
        ++it;
    }
}

void vehicle_horn_client_do_frame()
{
    if (rf::is_dedicated_server) {
        return;
    }
    vehicle_horn_report_edges();
    vehicle_horn_update_sounds();
}

void vehicle_horn_level_init()
{
    for (const auto& [handle, sound] : g_horn_sounds) {
        vehicle_horn_stop_sound(sound);
    }
    g_horn_sounds.clear();
}

void vehicle_horn_install()
{
    vehiclehorns_cmd.register_cmd();
}
