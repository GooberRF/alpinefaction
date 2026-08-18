#pragma once

#include <cstdint>
#include <vector>

namespace rf
{
    struct Player;
    struct Entity;
    struct Weapon;
    struct Vector3;
}

struct HillInfo;

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
    clutch = 13,
    revenge = 14,
    dominating = 15,
    rampage = 16,
};

constexpr uint8_t award_id_count = 17;

// Wire sentinel for "this award has no opposing player", in the award packet's victim id.
constexpr uint8_t award_no_victim = 0xFF;

void awards_do_patch();
// Everything the module tracks is per level; nemesis pairs deliberately survive Pit/Wipeout round
// boundaries, which do not reach here.
void awards_level_init();
// Player ids are reused, so a leaving player's state - including every nemesis pair it is part of -
// has to go with it.
void awards_on_player_destroy(rf::Player* player);

// -------------------------------------------------------------------------
// Server-side condition tracking
// -------------------------------------------------------------------------

// The one grant path: emits the afstats event, then notifies the earner (locally on a listen host,
// over the wire otherwise). Inert during pre-match. Exposed because a few awards are detected by
// the gametype that owns the rule rather than by a condition in this module.
void grant_award(rf::Player* recipient, AwardId id, rf::Player* victim = nullptr);

// Lethal transition, from entity_damage_hook. Runs for every death, killer included or not: the
// victim-side resets (streak, sniper/rail chain) must fire on world deaths and suicides too.
void awards_on_kill(rf::Player* victim, rf::Player* killer, int weapon_type, bool splash,
                    int killer_entity_handle);

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
// charge), so each explosion counts its own victims.
void awards_detonation_begin();
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
void awards_on_bagman_carrier_death(rf::Player* carrier, rf::Player* killer);
void awards_on_bagman_score_tick(rf::Player* carrier);

// Hill scoring. `team` is a HillOwner value (1 = red, 2 = blue).
void awards_on_hill_score_tick(const HillInfo& hill, int team);
void awards_on_hill_owner_change(const HillInfo& hill, int team, const std::vector<uint8_t>& player_ids);

// Which limit ended the game, latched where afstats is told the same thing. Clutch only resolves
// for a time-limit or score-limit end.
void awards_note_game_end_type(bool time_limit);
void awards_on_game_end();

// -------------------------------------------------------------------------
// Client-side display
// -------------------------------------------------------------------------

// Entry point for both wire paths: the af_sreq_award handler and the listen-host short circuit.
// `victim_player_id` is award_no_victim unless the award's callout names the opposing player.
void awards_client_on_award_received(uint8_t award_id, uint8_t victim_player_id);
// Drains the display queue, one award at a time. Ticked from the client frame.
void awards_client_do_frame();
