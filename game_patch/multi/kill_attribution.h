#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace rf
{
    struct Weapon;
}

// Publishes a projectile's weapon as the source of any radius damage applied inside the
// scope.
class SplashWeaponScope
{
public:
    explicit SplashWeaponScope(rf::Weapon* wp);
    ~SplashWeaponScope();

    SplashWeaponScope(const SplashWeaponScope&) = delete;
    SplashWeaponScope& operator=(const SplashWeaponScope&) = delete;

private:
    std::optional<int> prev_;
    bool active_ = false;
};

// What the server determined actually killed a player. Mirrors the kill-info wire payload
// minus the victim id, which is the map key on both sides of the wire.
struct KillAttribution
{
    uint8_t killer_player_id = 0xFF;
    uint8_t weapon_type = 0xFF; // 0xFF = unknown, callers fall back to the held-weapon heuristic
    uint8_t flags = 0;          // af_kill_info_flags
    uint8_t damage_type = 0xFF;
    std::vector<uint8_t> assist_player_ids; // most recent contributor first
};

// Weapon context of the obj_damage call currently being processed. weapon_type is
// -1 when the damage did not come from a weapon the server can name.
struct DamageWeaponContext
{
    int weapon_type = -1;
    bool splash = false;
};

constexpr int kill_attribution_hit_region_head = 2;
constexpr int kill_attribution_hit_region_legs = 0;

void kill_attribution_do_patch();
void kill_attribution_level_init();

DamageWeaponContext kill_attribution_get_damage_context();
// Hit region from the most recent get_hit_region_multiplier call, but only if that call was
// for `entity_handle`. -1 otherwise.
int kill_attribution_get_hit_region(int entity_handle);

// True when `weapon_type` is safe to use as an index into rf::weapon_types. Matters because
// the index reaches display code over the wire, where nothing constrains it.
bool kill_attribution_is_valid_weapon_type(int weapon_type);

// weapons.tbl index of the Riot Shield, or -1 when the loaded tables have no such weapon.
int kill_attribution_riot_shield_type();
// Riot Shield counts as melee even if a table forgets the melee flag (owner's decision).
bool kill_attribution_is_melee_weapon(int weapon_type);

// Combat chain feeding the assist list. Called for every PvP hit that did real damage.
void kill_attribution_note_pvp_damage(uint8_t victim_player_id, uint8_t attacker_player_id);
// Drains the victim's chain, dropping the killer and the victim themselves.
std::vector<uint8_t> kill_attribution_take_assists(uint8_t victim_player_id, uint8_t killer_player_id);

void kill_attribution_record(uint8_t killed_player_id, uint8_t killer_player_id, int weapon_type,
                             uint8_t flags, int damage_type, std::vector<uint8_t> assist_player_ids);

// For the kill-info packet. Non-consuming, because print_kill_message reads the same record
// afterwards for the server console line, but each recorded death is only ever returned once:
// a victim who dies again through a path that records nothing must not be announced twice
// with the same weapon and assists.
std::optional<KillAttribution> kill_attribution_lookup_for_send(uint8_t killed_player_id);
// Consuming: used when printing the kill message.
std::optional<KillAttribution> kill_attribution_consume(uint8_t killed_player_id);
