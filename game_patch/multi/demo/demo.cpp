#include <cstring>
#include <xlog/xlog.h>
#include "demo.h"
#include "demo_browser.h"
#include "demo_internal.h"
#include "../../os/os.h"
#include "../../rf/multi.h"

void demo_do_patch()
{
    demo_record_do_patch();
    demo_playback_do_patch();
    demo_powerup_timers_do_patch();
    demo_browser_apply_patch();
}

int demo_alloc_fake_reliable_socket(const rf::NetAddr& addr)
{
    // Stock psnet_rel_connect_to_server scans for a free slot starting at index 2
    int slot = -1;
    for (int i = 2; i < rf::NET_MAX_REL_SOCKETS; ++i) {
        if (rf::net_rel_sockets[i].status == rf::NetReliableSocketStatus::EMPTY) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        xlog::error("Demo: no free reliable socket slot");
        return -1;
    }
    rf::NetReliableSocket& sock = rf::net_rel_sockets[slot];
    std::memset(&sock, 0, sizeof(sock));
    sock.net_addr = addr;
    sock.status = rf::NetReliableSocketStatus::CONNECTED;
    const int now = static_cast<int>(timer::get_i64(1000));
    sock.last_packet_received = now;
    sock.last_packet_sent = now;
    sock.retransmission_timeout = 750;
    return slot;
}
