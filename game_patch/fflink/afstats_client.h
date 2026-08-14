#pragma once

// Set to false for release builds. Gates the stats verification logging across
// the afstats client, the server-side event sender, and the packet handlers.
// Note that PSSKs do appear in logs when this is true.
#define AFSTATS_VERIFICATION_LOGGING false

namespace rf
{
    struct NetAddr;
}

namespace fflink {

// A join_req is going out to `addr`. Anything tracked for a different server is
// dropped first, so a join the player cancelled can never hand its key to the next
// one. `stats_enabled` comes from the target's server browser entry and starts the
// PSK -> PSSK exchange; join_req resends do not start it twice.
void afstats_client_on_join_req(const rf::NetAddr& addr, bool stats_enabled);

// The join_accept has been parsed. This is where a direct connect - which has no
// browser entry when its join_req goes out - learns the server is stats-enabled.
void afstats_client_on_join_accept(bool stats_enabled);

// The client has entered the game and the reliable channel is up, so a PSSK that is
// already in hand can be delivered now.
void afstats_client_on_entered_game();

// Leaving multiplayer: invalidate anything still in flight and drop the PSSK.
void afstats_client_reset();

// Register console commands and any other one-time setup for the client stats
// subsystem. Called from fflink::do_patch().
void afstats_client_do_patch();

} // namespace fflink
