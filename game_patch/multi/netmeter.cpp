#include <algorithm>
#include <array>
#include <common/utils/int-utils.h>
#include <common/utils/list-utils.h>
#include <patch_common/FunHook.h>
#include "netmeter.h"
#include "multi.h"
#include "../misc/alpine_settings.h"
#include "../os/console.h"
#include "../rf/entity.h"
#include "../rf/multi.h"
#include "../rf/os/console.h"
#include "../os/os.h"
#include "../rf/player/player.h"

// ui_show_netmeter: client-side obj_update flow sampling. Packet timestamps are recorded by the
// hook below and by netmeter_record_out(); rate and jitter are derived on demand by
// netmeter_get_stats() for the HUD.

namespace
{

struct NetMeterChannel
{
    static constexpr int capacity = 256; // > 2 s at the highest netfps tier
    static constexpr int window_ms = 2000;
    std::array<int, capacity> times_ms{};
    int head = 0;
    int count = 0;

    void record(int now)
    {
        times_ms[head] = now;
        head = (head + 1) % capacity;
        count = std::min(count + 1, capacity);
    }

    struct Result
    {
        float rate = 0.0f;    // packets/s
        float jitter = 0.0f;  // mean abs deviation of inter-arrival time, ms
        float max_gap = 0.0f; // largest inter-arrival time, ms
        float missed = 0.0f;  // % of the expected packets that never arrived (not sent or lost)
        float stale = 0.0f;   // ms since the newest packet; 0 if none seen
    };

    // Stats over the recent window; zeros while the flow is absent or too sparse to measure.
    // nominal_ms is the expected packet interval for the missed estimate (0 = median gap).
    Result measure(int now, int nominal_ms) const
    {
        Result r;
        if (count > 0) {
            r.stale = static_cast<float>(now - times_ms[(head - 1 + capacity) % capacity]);
        }
        std::array<int, capacity> gaps;
        int n = 0, prev = 0, first = 0;
        for (int i = 0; i < count; ++i) {
            const int t = times_ms[(head - count + i + capacity) % capacity];
            if (now - t > window_ms) {
                continue;
            }
            if (n > 0) {
                gaps[n - 1] = t - prev;
            }
            else {
                first = t;
            }
            prev = t;
            ++n;
        }
        const int num_gaps = n - 1;
        const int span = prev - first;
        if (num_gaps < 2 || span <= 0) {
            return r;
        }
        const float avg_interval = static_cast<float>(span) / static_cast<float>(num_gaps);
        float dev_sum = 0.0f;
        int max_gap = 0;
        for (int i = 0; i < num_gaps; ++i) {
            const float dev = static_cast<float>(gaps[i]) - avg_interval;
            dev_sum += dev < 0.0f ? -dev : dev;
            max_gap = std::max(max_gap, gaps[i]);
        }
        r.rate = 1000.0f / avg_interval;
        r.jitter = dev_sum / static_cast<float>(num_gaps);
        r.max_gap = static_cast<float>(max_gap);
        if (nominal_ms <= 0) {
            // Stock or pre-1.5 server: take the median gap as the nominal tick
            std::nth_element(gaps.begin(), gaps.begin() + num_gaps / 2, gaps.begin() + num_gaps);
            nominal_ms = gaps[num_gaps / 2];
        }
        if (nominal_ms > 0) {
            const float expected = static_cast<float>(span) / static_cast<float>(nominal_ms);
            r.missed = std::max(0.0f, 100.0f * (expected - static_cast<float>(num_gaps)) / expected);
        }
        return r;
    }
};

NetMeterChannel g_netmeter_in;
NetMeterChannel g_netmeter_out;

} // namespace

FunHook<rf::MultiIoPacketHandler> process_obj_update_packet_netmeter_hook{
    0x0047DF90,
    [](char* data, const rf::NetAddr& addr) {
        if (!rf::is_server) {
            g_netmeter_in.record(static_cast<int>(timer::get_i64(1000)));
        }
        process_obj_update_packet_netmeter_hook.call_target(data, addr);
    },
};

void netmeter_record_out(int wire_ms)
{
    g_netmeter_out.record(wire_ms);
}

bool netmeter_enabled()
{
    return g_alpine_game_config.netmeter_display;
}

NetMeterStats netmeter_get_stats()
{
    // The display is refreshed on a fixed cadence, and the two values that move every frame are
    // aggregated over that interval: raw staleness is a sawtooth across the tick interval and interp
    // delay steps with every keyframe, so both are unreadable when drawn instantaneously.
    static constexpr int refresh_ms = 250;
    static NetMeterStats displayed{};
    static int last_refresh = 0;
    static float stale_peak = 0.0f;
    static float interp_sum = 0.0f;
    static int interp_samples = 0;

    const int now = static_cast<int>(timer::get_i64(1000));
    // Nominal tick: inbound is the server's tier (join_accept extension), outbound the fixed client
    // send rate; 0 lets the meter estimate it (stock/pre-1.5 server)
    const auto& info = get_af_server_info();
    const bool known = info && info->server_netfps;
    const auto in = g_netmeter_in.measure(now, known ? 1000 / info->server_netfps : 0);
    constexpr int client_interval_ms = 1000 / static_cast<int>(AlpineGameSettings::client_net_rate);
    const auto out = g_netmeter_out.measure(now, known ? client_interval_ms : 0);

    // Interp delay: how far behind the newest keyframe each remote entity is evaluated,
    // averaged over all remote players with interpolation state (16-bit server ms ticks;
    // negative while extrapolating past the newest keyframe)
    float interp_delay = 0.0f;
    int interp_count = 0;
    for (auto& player : SinglyLinkedList<rf::Player>{rf::player_list}) {
        if (&player == rf::local_player) {
            continue;
        }
        rf::Entity* ep = rf::entity_from_handle(player.entity_handle);
        if (ep && ep->obj_interp && ep->obj_interp->num_frames() > 0) {
            interp_delay += wrapped_diff16(ep->obj_interp->newest_frame_time(), ep->obj_interp->interp_time);
            ++interp_count;
        }
    }
    if (interp_count > 0) {
        interp_delay /= static_cast<float>(interp_count);
    }

    // Wire bandwidth from the engine's psnet stats (payload + approximate UDP/IP header per
    // datagram, both reliability classes): the last complete one-second slot
    float in_kbps = 0.0f, out_kbps = 0.0f;
    const int slot = std::min(rf::net_stats_slot, 29) - 1;
    if (slot >= 0) {
        const int in_bytes = rf::net_stats_recv_unrel[slot] + rf::net_stats_recv_rel[slot]
            + rf::net_stats_recv_hdr_unrel[slot] + rf::net_stats_recv_hdr_rel[slot];
        in_kbps = static_cast<float>(in_bytes) / 1024.0f;
        const int out_bytes = rf::net_stats_send_unrel[slot] + rf::net_stats_send_rel[slot]
            + rf::net_stats_send_hdr_unrel[slot] + rf::net_stats_send_hdr_rel[slot];
        out_kbps = static_cast<float>(out_bytes) / 1024.0f;
    }

    stale_peak = std::max(stale_peak, in.stale);
    interp_sum += interp_delay;
    ++interp_samples;
    if (now - last_refresh >= refresh_ms || now < last_refresh) {
        displayed = {in.rate, in.jitter, in.max_gap, in.missed, stale_peak, out.rate, out.jitter,
                     interp_sum / static_cast<float>(interp_samples), in_kbps, out_kbps};
        stale_peak = 0.0f;
        interp_sum = 0.0f;
        interp_samples = 0;
        last_refresh = now;
    }
    return displayed;
}

ConsoleCommand2 netmeter_cmd{
    "ui_show_netmeter",
    [] {
        g_alpine_game_config.netmeter_display = !g_alpine_game_config.netmeter_display;
        rf::console::print("Net meter display is {}", g_alpine_game_config.netmeter_display ? "enabled" : "disabled");
    },
    "Toggle the network meter (update rate, jitter, missed updates, interp delay, bandwidth)",
    "ui_show_netmeter",
};

void netmeter_apply_patches()
{
    process_obj_update_packet_netmeter_hook.install();
    netmeter_cmd.register_cmd();
}
