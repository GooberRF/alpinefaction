// The numeric-address check must match the engine's own inet_addr semantics,
// so silence MSVC's deprecation of it.
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <thread>
#include <winsock2.h>
#include <windns.h>
#include <xlog/xlog.h>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include "tracker.h"
#include "network.h"
#include "server_internal.h"
#include "../rf/multi.h"

CallHook<int(void*, int, int, rf::NetAddr&, int)> net_get_tracker_hook{
    0x00482ED4,
    [](void* data, int a2, int a3, rf::NetAddr& addr, int super_type) {
        int res = net_get_tracker_hook.call_target(data, a2, a3, addr, super_type);
        if (res != -1 && addr != rf::tracker_addr)
            res = -1;
        return res;
    },
};

FunHook<void()> tracker_do_broadcast_server_hook{
    0x00483130,
    []() {
        tracker_do_broadcast_server_hook.call_target();
        if (g_alpine_server_config.upnp_enabled) {
            // Auto forward server port using UPnP (in background thread)
            std::thread upnp_thread{try_to_auto_forward_port, rf::net_port};
            upnp_thread.detach();
        }
    },
};

// The stock game resolves the tracker hostname once at startup, so a tracker IP change only takes
// effect after a restart. Re-resolve it in the background when the DNS record's TTL expires.
struct TrackerDnsState
{
    std::atomic<bool> active{false};        // tracker is initialized and configured with a hostname
    std::atomic<bool> in_flight{false};     // background query is running
    std::atomic<bool> result_ready{false};  // query finished, waiting to be applied by the main thread
    std::atomic<uint32_t> result_ip{0};     // host byte order, 0 means the query failed
    std::atomic<uint32_t> result_ttl_s{0};
    std::chrono::steady_clock::time_point next_refresh{}; // main thread only
    bool failure_warned = false;                          // main thread only
};

static TrackerDnsState g_tracker_dns;

static void tracker_dns_resolve_worker(std::string host, uint32_t current_ip)
{
    uint32_t ip = 0;
    uint32_t ttl_s = 0;
    bool found = false;

    PDNS_RECORDA records = nullptr;
    if (DnsQuery_A(host.c_str(), DNS_TYPE_A, DNS_QUERY_STANDARD, nullptr, &records, nullptr) == ERROR_SUCCESS) {
        for (PDNS_RECORDA rec = records; rec; rec = rec->pNext) {
            if (rec->wType != DNS_TYPE_A) {
                continue;
            }
            const uint32_t rec_ip = ntohl(rec->Data.A.IpAddress); // tracker_addr keeps the address in host byte order
            // Blackhole and blocklist resolvers answer with 0.0.0.0, which collides with the failure sentinel
            if (rec_ip == 0) {
                continue;
            }
            if (!found) {
                ip = rec_ip;
                ttl_s = rec->dwTtl;
                found = true;
            }
            // Resolvers rotate multi-A answers, and flapping the tracker address drops in-flight
            // replies (they are validated against it), so stay on the current one while it is offered
            if (rec_ip == current_ip) {
                ip = rec_ip;
                ttl_s = rec->dwTtl;
                break;
            }
        }
        DnsRecordListFree(records, DnsFreeRecordList);
    }

    g_tracker_dns.result_ttl_s.store(ttl_s);
    g_tracker_dns.result_ip.store(ip);
    g_tracker_dns.result_ready.store(true);
    g_tracker_dns.in_flight.store(false);
}

FunHook<int()> tracker_init_hook{
    0x00482AE0,
    []() {
        int result = tracker_init_hook.call_target();
        // The engine initializes only once but its callers do not, so never re-arm and clobber the backoff
        if (result != 0 && !g_tracker_dns.active.load()) {
            // A numeric address has no DNS record that can expire, so only hostnames are worth
            // refreshing. Mirrors the stock game's test at 0x00482C4F.
            const bool is_numeric = inet_addr(rf::tracker_hostname) != INADDR_NONE;
            if (!is_numeric) {
                // The engine already resolved the address but exposes no TTL, so query once right away
                g_tracker_dns.next_refresh = std::chrono::steady_clock::now();
                g_tracker_dns.active.store(true);
            }
        }
        return result;
    },
};

FunHook<void()> tracker_do_frame_hook{
    0x00482D90,
    []() {
        tracker_do_frame_hook.call_target();

        if (!g_tracker_dns.active.load()) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        const char* const host = rf::tracker_hostname;

        // Sample in_flight BEFORE result_ready. The worker stores result_ready = true strictly before
        // in_flight = false, so in this order an observed in_flight == false guarantees any finished
        // worker's result is already visible in result_ready. Loading them the other way round lets a
        // worker that finishes between the two loads look idle, and a second worker gets spawned for
        // the same refresh.
        const bool in_flight = g_tracker_dns.in_flight.load();

        if (g_tracker_dns.result_ready.load()) {
            const uint32_t new_ip = g_tracker_dns.result_ip.load();
            if (new_ip != 0) {
                if (new_ip != rf::tracker_addr.ip_addr.inner) {
                    const rf::IpAddr old_ip = rf::tracker_addr.ip_addr;
                    rf::tracker_addr.ip_addr.inner = new_ip;
                    xlog::info("Tracker {} moved from {} to {}", host, old_ip, rf::IpAddr{new_ip});
                }
                const uint32_t ttl_s = std::clamp<uint32_t>(g_tracker_dns.result_ttl_s.load(), 60, 86400);
                g_tracker_dns.next_refresh = now + std::chrono::seconds{ttl_s};
                g_tracker_dns.failure_warned = false;
            }
            else {
                // A hostname that can never resolve would otherwise warn on every retry forever
                if (!g_tracker_dns.failure_warned) {
                    xlog::warn("Failed to re-resolve tracker {}, keeping {}", host, rf::tracker_addr.ip_addr);
                    g_tracker_dns.failure_warned = true;
                }
                else {
                    xlog::debug("Failed to re-resolve tracker {}, keeping {}", host, rf::tracker_addr.ip_addr);
                }
                g_tracker_dns.next_refresh = now + std::chrono::seconds{120};
            }
            g_tracker_dns.result_ready.store(false);
        }
        else if (!in_flight && now >= g_tracker_dns.next_refresh) {
            g_tracker_dns.in_flight.store(true);
            try {
                std::thread dns_thread{tracker_dns_resolve_worker, std::string{host},
                    rf::tracker_addr.ip_addr.inner};
                dns_thread.detach();
            }
            catch (const std::exception& e) {
                g_tracker_dns.in_flight.store(false);
                xlog::warn("Failed to spawn tracker DNS resolve thread: {}", e.what());
                g_tracker_dns.next_refresh = now + std::chrono::seconds{120};
            }
        }
    },
};

void tracker_do_patch()
{
    // Make sure tracker packets come from configured tracker
    net_get_tracker_hook.install();

    // Use UPnP for port forwarding if server is not in LAN-only mode
    tracker_do_broadcast_server_hook.install();

    // Re-resolve the tracker hostname when its DNS record expires
    tracker_init_hook.install();
    tracker_do_frame_hook.install();
}
