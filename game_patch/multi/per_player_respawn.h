#pragma once

// Per-player weapon respawn (`weapons_stay = "per_player"`): every placed weapon stays for everyone,
// and each player has their own cooldown on each copy they take. The server enforces it; the
// taker's client draws the copy as a ghost until its cooldown runs out. Dying clears them all.

namespace rf
{
    struct Item;
    struct Player;
    struct GRoom;
}

// Bound on any single cooldown, from config or the wire.
constexpr int per_player_respawn_max_ms = 3600 * 1000;

// Server: per-player respawn is on and governs this item (a placed weapon outside the stay exemptions).
bool per_player_respawn_applies(const rf::Item* item);
bool weapon_stay_is_exempt(int weapon_type);
// Server: the player is still cooling down on this copy.
bool per_player_respawn_is_blocked(rf::Player* player, const rf::Item* item);
// Either side: this player may not take this copy yet (the server's cooldown, or the local
// player's ghost on a client).
bool per_player_respawn_blocks(rf::Player* player, const rf::Item* item);
// Server: after a confirmed pickup.
void per_player_respawn_on_picked_up(rf::Player* player, rf::Item* item);
// Server and client: the player died.
void per_player_respawn_on_player_killed(rf::Player* player);
// Server and client: a new level started.
void per_player_respawn_level_init();

// Client: the server says the local player took this item.
void per_player_respawn_client_on_taken(int item_handle, int remaining_ms);
// Client: the local player sees this item as a ghost.
bool per_player_respawn_is_ghosted(const rf::Item* item);
// Client: draws the ghosts of the items in this room, during its render pass.
void per_player_respawn_render_pass(rf::GRoom* room_filter);

void per_player_respawn_apply_patch();
