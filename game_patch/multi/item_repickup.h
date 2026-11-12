#pragma once

// Weapon re-pickup (`allow_repickup` with weapon stay): every placed weapon stays for everyone,
// and each player has their own cooldown on each copy they take. The server enforces it; the
// taker's client draws the copy as a ghost until its cooldown runs out. Dying clears them all.

namespace rf
{
    struct Item;
    struct Player;
    struct GRoom;
}

// Bound on any single cooldown, from config or the wire.
constexpr int item_repickup_max_timeout_ms = 3600 * 1000;

// Server: re-pickup is on and governs this item (a placed weapon outside the stay exemptions).
bool item_repickup_applies(const rf::Item* item);
bool weapon_stay_is_exempt(int weapon_type);
// Server: the player is still cooling down on this copy.
bool item_repickup_is_blocked(rf::Player* player, const rf::Item* item);
// Either side: this player may not take this copy yet (the server's cooldown, or the local
// player's ghost on a client).
bool item_repickup_blocks(rf::Player* player, const rf::Item* item);
// Server: after a confirmed pickup.
void item_repickup_on_picked_up(rf::Player* player, rf::Item* item);
// Server and client: the player died.
void item_repickup_on_player_killed(rf::Player* player);
// Server and client: a new level started.
void item_repickup_level_init();

// Client: the server says the local player took this item.
void item_repickup_client_on_taken(int item_handle, int remaining_ms);
// Client: the local player sees this item as a ghost.
bool item_repickup_is_ghosted(const rf::Item* item);
// Client: draws the ghosts of the items in this room, during its render pass.
void item_repickup_render_pass(rf::GRoom* room_filter);

void item_repickup_apply_patch();
