#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include "../rf/multi.h"

struct TrackerDnsAnswer
{
    uint32_t ip = 0; // host byte order, 0 on failure
    uint32_t ttl_s = 0;
};

// Blocking. Prefers `current_ip` among several answers, so a rotating record does not move the tracker.
TrackerDnsAnswer tracker_dns_resolve(const std::string& host, uint32_t current_ip);
// The engine's tracker address, kept fresh for its hostname, once tracker_init has succeeded for `host`.
std::optional<rf::NetAddr> tracker_resolved_addr(std::string_view host);

void tracker_do_patch();
