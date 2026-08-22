#pragma once

#include <cstdint>

namespace rf
{
    struct Player;
    struct Entity;
    struct Weapon;
    struct Vector3;
}

// Wire-frozen registry: the id is what goes on the af_sreq_award packet and into the afstats
// `award` event, so entries are APPEND-ONLY and existing values never change meaning. The client
// owns the text and sound for each id (see the table in awards.cpp).
enum class AwardId : uint8_t
{
    toasty = 0,
    massacre = 1,
    riot_control = 2,
    twofer = 3,
    impressive = 4,
    excellent = 5,
    overkill = 6,
    unstoppable = 7,
    last_stand = 8,
    flag_runner = 9,
    capture_denied = 10,
    area_denied = 11,
    bag_check = 12,
    revenge = 13,
    dominating = 14,
    rampage = 15,
    first_blood = 16,
    hat_trick = 17,
    airshot = 18,
    clean_sweep = 19,
    quickdraw = 20,
    stonewall = 21,
    lockdown = 22,
    hazard_pay = 23,
    last_laugh = 24,
    x_ray = 25,
    depth_charge = 26,
    censored = 27,
};

constexpr uint8_t award_id_count = 28;

// Wire sentinel for "this award has no opposing player", in the award packet's victim id.
constexpr uint8_t award_no_victim = 0xFF;

void awards_do_patch();
// Everything the module tracks is per level; nemesis pairs deliberately survive Pit/Wipeout round
// boundaries, which do not reach here.
void awards_level_init();
// Player ids are reused, so we need to remove them from tracking.
void awards_on_player_destroy(rf::Player* player);

// -------------------------------------------------------------------------
// Server-side condition tracking
// -------------------------------------------------------------------------

// The one grant path: emits the afstats event, then notifies the earner (locally on a listen host,
// over the wire otherwise). Inert during pre-match. Exposed because a few awards are detected by
// the gametype that owns the rule rather than by a condition in this module.
void grant_award(rf::Player* recipient, AwardId id, rf::Player* victim = nullptr);

// Server frame tick. Samples the per-player state that only exists as a live flag..
void awards_server_do_frame();

// Lethal transition, from entity_damage_hook. Runs for every death, killer included or not: the
// victim-side resets (streak, sniper/rail chain) must fire on world deaths and suicides too.
// `victim_team` is the team the victim had when the damage landed - death processing can move
// them (auto team balance) before this runs. `damage` is the killing event's post-modifier damage;
// `victim_life` and `victim_armor` are the victim's values from before that hit landed.
void awards_on_kill(rf::Player* victim, rf::Player* killer, int weapon_type, bool splash,
                    int killer_entity_handle, int victim_team, float damage, float victim_life,
                    float victim_armor);

// Any counted shot leaving a player's weapon, from the afstats fired sites.
void awards_on_weapon_fired(rf::Player* player, int weapon_type);
// A direct weapon hit on another player that the accuracy path counted (one per rail bolt).
void awards_on_direct_hit(rf::Player* attacker, rf::Player* victim, int weapon_type);

// One lag-compensated shot's whole resolution, so the kills of a single sniper/rail bolt can be
// grouped. Mirrors AccuracyShotScope, which is opened at the same call.
class AwardsShotScope
{
public:
    AwardsShotScope(rf::Entity* shooter, rf::Weapon* wp);
    ~AwardsShotScope();

    AwardsShotScope(const AwardsShotScope&) = delete;
    AwardsShotScope& operator=(const AwardsShotScope&) = delete;

private:
    bool active_ = false;
};

// One detonation's kills, driven from SplashWeaponScope. Scopes nest (a rocket setting off a remote
// charge), so each explosion counts its own victims. The weapon lets the detonation's own direct-hit
// kill count toward it alongside the splash kills.
void awards_detonation_begin(int weapon_type);
void awards_detonation_end();

// Flag carrying. CTF distinguishes a steal from a ground pickup by `from_base`; Salvage by whether
// the flag was taken from its spawn.
void awards_on_ctf_flag_taken(rf::Player* player, bool red_flag, bool from_base, const rf::Vector3& pos);
void awards_on_ctf_flag_dropped(rf::Player* player);
void awards_on_ctf_capture(rf::Player* player);
void awards_on_sal_flag_taken(rf::Player* player, bool from_spawn);
void awards_on_sal_flag_dropped(rf::Player* player);
void awards_on_sal_capture(rf::Player* player);

void awards_on_bagman_pickup(rf::Player* player);
// The bag left this player, however it left them: a Hazard Pay run only counts uninterrupted time.
void awards_on_bagman_drop(rf::Player* player);
// The carrier still holds the bag this frame, from bagman's server update.
void awards_on_bagman_hold_tick(rf::Player* carrier);
void awards_on_bagman_carrier_death(rf::Player* carrier, rf::Player* killer);

// A spray the server actually accepted and applied.
void awards_on_spray(rf::Player* player);

// -------------------------------------------------------------------------
// Client-side display
// -------------------------------------------------------------------------

// Entry point for both wire paths: the af_sreq_award handler and the listen-host short circuit.
// `victim_player_id` is award_no_victim unless the award's callout names the opposing player.
void awards_client_on_award_received(uint8_t award_id, uint8_t victim_player_id);
// Drains the display queue, one award at a time. Ticked from the client frame.
void awards_client_do_frame();
