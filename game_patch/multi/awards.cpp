#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <chrono>
#include <deque>
#include <format>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include <common/utils/list-utils.h>
#include <common/utils/string-utils.h>
#include "../rf/collide.h"
#include "../rf/entity.h"
#include "../rf/geometry.h"
#include "../rf/math/vector.h"
#include "../rf/multi.h"
#include "../rf/object.h"
#include "../rf/os/timestamp.h"
#include "../rf/player/player.h"
#include "../rf/weapon.h"
#include "../fflink/afstats_events.h"
#include "../hud/hud.h"
#include "../misc/alpine_settings.h"
#include "../misc/player.h"
#include "../os/console.h"
#include "../sound/sound.h"
#include "alpine_packets.h"
#include "awards.h"
#include "bagman.h"
#include "demo/demo.h"
#include "gametype.h"
#include "kill_attribution.h"
#include "multi.h"
#include "mutators.h"
#include "payload.h"
#include "powerups.h"
#include "salvage.h"
#include "scavenger.h"
#include "server.h"
#include "server_internal.h"
#include "vehicles/vehicle.h"
#include "../misc/settings_registry.h"

// Server-detected per-player awards. The server owns every condition below; the client is told an
// id and owns the text and the sound for it, so adding an award never needs a wire change.
// Awards are disabled during pre-match.

namespace
{

using Clock = std::chrono::steady_clock;

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

bool awards_tracking_active()
{
    return rf::is_multi && rf::is_server && !g_match_info.pre_match_active;
}

int player_id_of(const rf::Player* player)
{
    if (!player || !player->net_data) {
        return -1;
    }
    const int id = player->net_data->player_id;
    return (id >= 0 && id < rf::multi_max_player_id) ? id : -1;
}

// "Enemy" excludes teammates in team modes and excludes the player themselves everywhere, so
// team kills and suicides advance nothing. The victim's team is a parameter because on the kill
// path it has to be the team held when the damage landed, not whatever death processing left.
bool is_enemy_kill(const rf::Player* killer, const rf::Player* victim, int victim_team)
{
    if (!killer || !victim || killer == victim) {
        return false;
    }
    if (multi_is_team_game_type() && killer->team == victim_team) {
        return false;
    }
    return true;
}

bool is_sniper_or_rail(int weapon_type)
{
    if (!kill_attribution_is_valid_weapon_type(weapon_type)) {
        return false;
    }
    return weapon_type == rf::sniper_rifle_weapon_type || weapon_type == rf::rail_gun_weapon_type;
}

bool is_explosive_weapon(int weapon_type)
{
    return kill_attribution_is_valid_weapon_type(weapon_type)
        && rf::weapon_types[weapon_type].damage_radius > 0.0f;
}

bool is_scoped_weapon(int weapon_type)
{
    if (!kill_attribution_is_valid_weapon_type(weapon_type)) {
        return false;
    }
    return weapon_type == rf::sniper_rifle_weapon_type
        || weapon_type == rf::rail_gun_weapon_type
        || weapon_type == rf::scope_assault_rifle_weapon_type;
}

bool player_is_zoomed(const rf::Player* player)
{
    return player && (player->fpgun_data.zooming_in || player->fpgun_data.scanning_for_target);
}

// scanning_for_target is only reliable for weapons with scanners (ie. rail).
bool player_scanner_engaged(const rf::Player* player, const rf::Entity* ep)
{
    return player && ep && player->fpgun_data.scanning_for_target
        && rf::weapon_has_scanner(ep->ai.current_primary_weapon);
}

// Whoever is running the mode's objective: a flag or cartridge carrier, or an attacker pushing the cart.
bool player_is_objective_runner(const rf::Player* player, int player_team)
{
    if (rf::multi_get_game_type() == rf::NG_TYPE_CTF) {
        return rf::multi_ctf_get_red_flag_player() == player ||
        rf::multi_ctf_get_blue_flag_player() == player;
    }
    if (payload_active()) {
        return player_team == payload_attacking_team() && payload_player_in_push_zone(*player)
            && !payload_in_setup() && !payload_finale_reached();
    }
    return salvage_player_is_carrier(player);
}

// -------------------------------------------------------------------------
// Per-player state. Indexed by player id, which the engine REUSES - see
// awards_on_player_destroy.
// -------------------------------------------------------------------------

constexpr int massacre_kills_needed = 8;
constexpr int64_t excellent_window_ms = 3000;
constexpr int excellent_kills_needed = 3;
constexpr int unstoppable_streak_step = 10;
constexpr float overkill_min_damage = 300.0f;
constexpr float overkill_max_victim_life = 10.0f;
constexpr float last_stand_life = 2.0f;
constexpr float denied_base_radius = 5.0f;
constexpr int area_denied_capture_milli = 98000;
constexpr int64_t bag_check_window_ms = 1000;
constexpr uint8_t nemesis_kills_needed = 5;
constexpr int hat_trick_caps_needed = 3;
constexpr int stonewall_kills_needed = 5;
constexpr int wrecking_crew_kills_needed = 2;
constexpr int power_trip_kills_needed = 5;
constexpr int stacked_powerups_needed = 3;
constexpr float mechanic_repair_needed = 1500.0f;
constexpr int clean_sweep_min_roster = 2;
constexpr int64_t hazard_pay_hold_ms = 60000;
constexpr int64_t censored_window_ms = 1000;
constexpr int64_t x_ray_scanner_lockout_ms = 5000;
constexpr int shotgun_volley_kills_needed = 2;
constexpr float airshot_clearance = 1.6f;
constexpr int award_trace_flags = rf::CF_ANY_HIT | rf::CF_PROCESS_INVISIBLE_FACES;
// One detonation resolves all its deaths in a single frame, so the per-kill awards (Last Stand,
// Riot Control, ...) would otherwise land once per victim. The same award within this window is one
// achievement, not several. Kept to a few frames on purpose: kills a human lands in genuine quick
// succession are separate events and must all count.
constexpr int64_t award_repeat_window_ms = 50;

struct AwardPlayerState
{
    // Riot Control: which halves of the stick+shield pair have landed this life.
    bool riot_stick_kill = false;
    bool riot_shield_kill = false;
    // Impressive: a sniper/rail shot is in flight, and the length of the current consecutive-hit
    // chain.
    bool shot_pending = false;
    int hit_chain = 0;
    // Excellent: timestamps of the recent kills. A fixed window on purpose: 256 of these live in
    // a flat array, so the state may not heap-allocate.
    std::array<int64_t, excellent_kills_needed> excellent_kills{};
    int excellent_kill_count = 0;
    // Unstoppable. Deliberately the module's own counter: the scoreboard and afstats
    // counters are written by other paths with their own gating.
    int streak = 0;
    // Flag Runner: this player stole the flag and has neither died nor dropped it since.
    bool clean_steal = false;
    // Bag Check: when this player last took the bag. Hazard Pay latches off the same run.
    bool has_bag_pickup = false;
    int64_t bag_pickup_ms = 0;
    bool hazard_pay_granted = false;
    // Hat Trick: captures since this life began.
    int caps_this_life = 0;
    // Stonewall: objective runners killed this life.
    int carrier_kills = 0;
    // Clean Sweep: the enemies killed this life, and whether the sweep already paid out. A bitset,
    // not a container: 256 of these live in a flat array, so the state may not heap-allocate.
    std::bitset<rf::multi_max_player_id> kills_this_life;
    bool clean_sweep_granted = false;
    // Censored: when this player last put up a spray.
    int64_t last_spray_ms = 0;
    // X-Ray: when the rail scanner was last seen engaged.
    int64_t last_scanner_ms = 0;
    // 2fer: the shotgun volley being counted.
    int64_t last_shotgun_kill_ms = 0;
    int shotgun_volley_kills = 0;
    // Grand Theft Auto: the hull this player took from the other team this life, or -1.
    int stolen_hull_handle = -1;
    // Power Trip and Stacked, as afstats::Powerup bits: the timed powerups running from a pickup this
    // life, and which pickups have paid out. The kill counts are per pickup and restart with each one.
    uint8_t power_pickup_mask = 0;
    uint8_t power_trip_granted_mask = 0;
    std::array<uint8_t, afstats::powerup_count> power_trip_kills{};
    // Mechanic: hull life repaired this life since its last payout.
    float repaired_this_life = 0.0f;
    // Per-award repeat throttle; 0 = never granted.
    std::array<int64_t, award_id_count> last_grant_ms{};
};

std::vector<AwardPlayerState>& player_states()
{
    static std::vector<AwardPlayerState> states(rf::multi_max_player_id);
    return states;
}

AwardPlayerState* state_for(const rf::Player* player)
{
    const int id = player_id_of(player);
    return id < 0 ? nullptr : &player_states()[id];
}

// First Blood: one per level.
bool g_first_blood_claimed = false;

// -------------------------------------------------------------------------
// Nemesis (a status, not an award) and the Revenge award it enables.
// Keyed by the ORDERED pair (killer, victim).
// -------------------------------------------------------------------------

struct NemesisPair
{
    uint8_t count = 0;
    bool active = false;
};

uint16_t nemesis_key(uint8_t killer_id, uint8_t victim_id)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(killer_id) << 8) | victim_id);
}

std::map<uint16_t, NemesisPair> g_nemesis;

// -------------------------------------------------------------------------
// Detonation frames (Massacre). One frame per open SplashWeaponScope; they nest, so chained
// explosions are separate detonations.
// -------------------------------------------------------------------------

struct DetonationKills
{
    uint8_t killer_id = 0xFF;
    int count = 0;
    rf::Player* last_victim = nullptr;
};

struct DetonationFrame
{
    int weapon_type = -1;
    std::vector<DetonationKills> killers;
};

// Kept as a stack with retained capacity: the scope is constructed very often, so neither entering
// nor leaving one may allocate in the common (no kills) case.
std::vector<DetonationFrame> g_detonation_stack;
int g_detonation_depth = 0;

// -------------------------------------------------------------------------
// Vehicle blast frames (Scrap Metal, Wrecking Crew, Air Superiority). One frame per hull whose
// occupant blast is resolving, stacked in case a rider's death destroys another hull meanwhile.
// -------------------------------------------------------------------------

struct VehicleBlastFrame
{
    int hull_handle = -1;
    int hull_class = -1;
    rf::Player* destroyer = nullptr;
    int destroyer_vehicle_class = -1;
    // Counted as the hull dies, not from the kills: a rider can survive the blast.
    int enemy_occupants = 0;
    std::vector<DetonationKills> killers;
};

std::vector<VehicleBlastFrame> g_vehicle_blast_stack;

// -------------------------------------------------------------------------
// Lag-compensated shot scope (2fer). One scope is one fired projectile: sniper and rail both
// create a single bullet per shot, and the whole flight - every pierced victim - resolves inside
// this call.
// -------------------------------------------------------------------------

struct ShotScopeState
{
    bool active = false;
    int shooter_id = -1;
    int weapon_type = -1;
    int enemy_kills = 0;
    rf::Player* last_victim = nullptr;
};

ShotScopeState g_shot_scope;

// -------------------------------------------------------------------------
// CTF flag stands. When a flag is home its position IS the stand position, so the cache is filled
// opportunistically from every moment one is observed at home.
// -------------------------------------------------------------------------

struct FlagHomeCache
{
    bool known = false;
    rf::Vector3 pos{};
};

FlagHomeCache g_flag_home[2]; // 0 = red, 1 = blue

void note_flag_home(int team, const rf::Vector3& pos)
{
    if (team != 0 && team != 1) {
        return;
    }
    g_flag_home[team].known = true;
    g_flag_home[team].pos = pos;
}

void sample_ctf_flag_homes()
{
    if (rf::multi_get_game_type() != rf::NG_TYPE_CTF) {
        return;
    }
    // The in-base getters are null-item safe (multi_ctf_is_*_flag_in_base_hook) and the position
    // getters copy from globals, so a one-flag level cannot reach a null item through here.
    rf::Vector3 pos{};
    if (rf::multi_ctf_is_red_flag_in_base()) {
        rf::multi_ctf_get_red_flag_pos(&pos);
        note_flag_home(0, pos);
    }
    if (rf::multi_ctf_is_blue_flag_in_base()) {
        rf::multi_ctf_get_blue_flag_pos(&pos);
        note_flag_home(1, pos);
    }
}

} // namespace

// -------------------------------------------------------------------------
// Granting
// -------------------------------------------------------------------------

void grant_award(rf::Player* recipient, AwardId id, rf::Player* victim)
{
    if (!recipient || !awards_tracking_active()) {
        return;
    }

    // The same award twice in one instant is one achievement (an explosion's eight deaths all
    // resolve in the same frame). Suppressed entirely: the stats event is a duplicate too.
    if (AwardPlayerState* state = state_for(recipient)) {
        int64_t& last = state->last_grant_ms[static_cast<size_t>(id)];
        const int64_t now = now_ms();
        if (last != 0 && now - last < award_repeat_window_ms) {
            return;
        }
        last = now;
    }

    afstats::on_award(recipient, static_cast<uint8_t>(id), victim);

    const int victim_id = player_id_of(victim);
    const uint8_t wire_victim =
        victim_id >= 0 ? static_cast<uint8_t>(victim_id) : award_no_victim;
    const auto notify = [&](rf::Player* target) {
        if (target == rf::local_player) {
            // Listen-server host: render locally instead of routing through the network path.
            awards_client_on_award_received(static_cast<uint8_t>(id), wire_victim);
        }
        else {
            af_send_award(target, static_cast<uint8_t>(id), wire_victim);
        }
    };

    // Bots earn awards, they just have nothing to show them on - but somebody spectating one does.
    if (!recipient->is_bot && !recipient->is_browser) {
        notify(recipient);
    }

    // Mirror to anyone watching the earner, the same way a damage notification is mirrored to the
    // dealer's spectators in entity_damage_hook. af_send_award applies the version gate and the
    // bot/browser skip itself.
    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (!player.net_data || &player == recipient) {
            continue;
        }
        if (player_is_spectating(player, recipient)) {
            notify(&player);
        }
    }

    // Mirror to the demo recorder tagged with the earner so playback shows it to whoever
    // is spectating the earner.
    if (demo_record_recorder()) {
        const int earner_id = player_id_of(recipient);
        if (earner_id >= 0) {
            demo_record_award(static_cast<uint8_t>(id), wire_victim, static_cast<uint8_t>(earner_id));
        }
    }
}

namespace
{

// -------------------------------------------------------------------------
// Individual conditions
// -------------------------------------------------------------------------

void check_riot_control(rf::Player* killer, rf::Player* victim, int weapon_type)
{
    if (!kill_attribution_is_valid_weapon_type(weapon_type)) {
        return;
    }
    AwardPlayerState* state = state_for(killer);
    if (!state) {
        return;
    }
    const int shield = kill_attribution_riot_shield_type();
    if (rf::weapon_is_riot_stick(weapon_type)) {
        state->riot_stick_kill = true;
    }
    else if (shield >= 0 && weapon_type == shield) {
        state->riot_shield_kill = true;
    }
    else {
        return;
    }
    if (state->riot_stick_kill && state->riot_shield_kill) {
        // Cleared, not kept: the next award needs a fresh pair.
        state->riot_stick_kill = false;
        state->riot_shield_kill = false;
        grant_award(killer, AwardId::riot_control, victim);
    }
}

void check_excellent(rf::Player* killer, rf::Player* victim)
{
    AwardPlayerState* state = state_for(killer);
    if (!state) {
        return;
    }
    const int64_t now = now_ms();
    // Compact the window in place, dropping anything older than 3 s (and, should the window
    // somehow be full, the oldest entry - it can never matter which, a grant empties it).
    int kept = 0;
    for (int i = 0; i < state->excellent_kill_count; ++i) {
        if (now - state->excellent_kills[i] <= excellent_window_ms
            && kept < excellent_kills_needed - 1) {
            state->excellent_kills[kept++] = state->excellent_kills[i];
        }
    }
    state->excellent_kills[kept++] = now;
    state->excellent_kill_count = kept;
    if (state->excellent_kill_count >= excellent_kills_needed) {
        // Cleared, not trimmed: the next award needs three fresh kills.
        state->excellent_kill_count = 0;
        grant_award(killer, AwardId::excellent, victim);
    }
}

void check_streak(rf::Player* killer, rf::Player* victim)
{
    AwardPlayerState* state = state_for(killer);
    if (!state) {
        return;
    }
    ++state->streak;
    if (state->streak % unstoppable_streak_step == 0) {
        grant_award(killer, AwardId::unstoppable, victim);
    }
}

// Overkill: a hit big enough to have killed the victim many times over.
void check_overkill(rf::Player* killer, rf::Player* victim, float damage, float victim_life, float victim_armor)
{
    if (victim_life <= overkill_max_victim_life && victim_armor <= 0.0f && damage >= overkill_min_damage) {
        grant_award(killer, AwardId::overkill, victim);
    }
}

void check_last_stand(rf::Player* killer, rf::Player* victim, int killer_entity_handle)
{
    rf::Entity* killer_ep = rf::entity_from_handle(killer_entity_handle);
    if (!killer_ep) {
        return;
    }
    // A trade leaves the killer's entity resolvable but dead; surviving on a sliver is the whole
    // point of the award, so a posthumous kill earns nothing.
    if (killer_ep->life <= 0.0f || rf::entity_is_dying(killer_ep)) {
        return;
    }
    if (killer_ep->life < last_stand_life && killer_ep->armor <= 0.0f) {
        grant_award(killer, AwardId::last_stand, victim);
    }
}

// CTF: the victim delivers to their OWN team's flag stand, and only if a capture would actually
// happen there at that instant.
bool ctf_capture_denied(rf::Player* killer, rf::Player* victim, int victim_team_raw, rf::Vector3* out_base)
{
    if (rf::multi_get_game_type() != rf::NG_TYPE_CTF) {
        return false;
    }
    // The carried flag has to be the killer's team's, i.e. the one this victim scores with.
    rf::Player* const red_carrier = rf::multi_ctf_get_red_flag_player();
    rf::Player* const blue_carrier = rf::multi_ctf_get_blue_flag_player();
    const bool carries_red = red_carrier == victim;
    const bool carries_blue = blue_carrier == victim;
    if (!carries_red && !carries_blue) {
        return false;
    }
    const int stolen_flag_team = carries_red ? 0 : 1;
    if (killer->team != stolen_flag_team) {
        return false;
    }

    const int victim_team = victim_team_raw == 0 ? 0 : 1;
    const bool own_flag_home =
        victim_team == 0 ? rf::multi_ctf_is_red_flag_in_base() : rf::multi_ctf_is_blue_flag_in_base();
    if (!own_flag_home && !g_alpine_server_config_active_rules.flag_captures_while_stolen) {
        return false; // a touch would not capture, so nothing was denied
    }

    if (own_flag_home) {
        rf::Vector3 pos{};
        if (victim_team == 0) {
            rf::multi_ctf_get_red_flag_pos(&pos);
        }
        else {
            rf::multi_ctf_get_blue_flag_pos(&pos);
        }
        note_flag_home(victim_team, pos);
    }
    if (!g_flag_home[victim_team].known) {
        return false;
    }
    *out_base = g_flag_home[victim_team].pos;
    return true;
}

bool sal_capture_denied(rf::Player* killer, rf::Player* victim, int victim_team, rf::Vector3* out_base,
                        float* out_radius)
{
    if (!salvage_player_is_carrier(victim)) {
        return false;
    }
    if (multi_is_team_game_type() && killer->team == victim_team) {
        return false;
    }
    if (gt_is_scavenger()) {
        // A carrier who reaches the zone alive captures that frame, so the margin starts at its edge.
        if (!scavenger_get_zone(out_base)) {
            return false;
        }
        *out_radius = scavenger_get_zone_radius() + denied_base_radius;
        return true;
    }
    rf::Vector3 red_base{};
    rf::Vector3 blue_base{};
    if (!salvage_get_base_positions(&red_base, &blue_base)) {
        return false;
    }
    // Salvage always allows a capture, so proximity to the victim's own base is the whole test.
    *out_base = victim_team == 0 ? red_base : blue_base;
    return true;
}

void check_capture_denied(rf::Player* killer, rf::Player* victim, int victim_team)
{
    rf::Vector3 base{};
    float radius = denied_base_radius;
    if (!ctf_capture_denied(killer, victim, victim_team, &base)
        && !sal_capture_denied(killer, victim, victim_team, &base, &radius)) {
        return;
    }

    // The carrier's own position at the moment of death; the killer may be anywhere.
    rf::Entity* victim_ep = rf::entity_from_handle(victim->entity_handle);
    if (!victim_ep) {
        return;
    }
    if ((victim_ep->pos - base).len() <= radius) {
        grant_award(killer, AwardId::capture_denied, victim);
    }
}

void check_area_denied(rf::Player* killer, rf::Player* victim, int victim_team_raw)
{
    if (!multi_game_type_has_hills(rf::multi_get_game_type())) {
        return;
    }
    const HillOwner victim_team = victim_team_raw == 0 ? HillOwner::HO_Red : HillOwner::HO_Blue;
    const HillOwner killer_team = killer->team == 0 ? HillOwner::HO_Red : HillOwner::HO_Blue;

    for (const HillInfo& hill : g_koth_info.hills) {
        if (!hill.trigger || !hill.handler) {
            continue;
        }
        // The hill has to be one that can actually be taken right now. A locked or permalocked hill
        // keeps whatever capture_milli and steal_dir it had when it locked - update_hill_server
        // returns early for those without clearing either - so stale 99% progress sits on it for
        // the rest of the level and would otherwise qualify every kill on top of it.
        if (hill.lock_status != HillLockStatus::HLS_Available) {
            continue;
        }
        // ESC additionally gates by role: a hill whose prerequisite the victim's team has not taken
        // is not attackable by them, so progress there is not progress toward anything.
        if (gt_is_esc() && !esc_team_can_attack_hill(hill, victim_team)) {
            continue;
        }
        // Progress only counts when it belongs to the victim's team: steal_dir is who the hill is
        // currently leaning toward, whatever the mode's ownership rules are.
        if (hill.steal_dir != victim_team || hill.capture_milli <= area_denied_capture_milli) {
            continue;
        }
        if (!player_inside_hill_trigger(hill, *victim)) {
            continue;
        }

        bool other_teammate_on_hill = false;
        bool enemy_on_hill = false;
        for (rf::Player& other : SinglyLinkedList{rf::player_list}) {
            if (&other == victim || !player_is_countable(other)) {
                continue;
            }
            const HillOwner other_team = other.team == 0 ? HillOwner::HO_Red : HillOwner::HO_Blue;
            if (other_team != victim_team && other_team != killer_team) {
                continue;
            }
            if (!player_inside_hill_trigger(hill, other)) {
                continue;
            }
            if (other_team == victim_team) {
                other_teammate_on_hill = true;
                break;
            }
            enemy_on_hill = true;
        }
        if (other_teammate_on_hill || enemy_on_hill) {
            continue;
        }

        grant_award(killer, AwardId::area_denied, victim);
        return;
    }
}

void check_airshot(rf::Player* killer, rf::Player* victim, int weapon_type, bool splash)
{
    if (splash || !is_explosive_weapon(weapon_type)) {
        return;
    }
    rf::Entity* victim_ep = rf::entity_from_handle(victim->entity_handle);
    if (!victim_ep || rf::entity_on_ground(victim_ep) || rf::entity_is_swimming(victim_ep)) {
        return;
    }

    rf::Vector3 p0 = victim_ep->pos;
    rf::Vector3 p1 = p0;
    p1.y -= victim_ep->p_data.radius + airshot_clearance;
    rf::GCollisionOutput collision{};
    if (rf::collide_linesegment_level_solid(p0, p1, award_trace_flags, &collision)) {
        return;
    }
    grant_award(killer, AwardId::airshot, victim);
}

// Roster membership, not aliveness.
bool counts_toward_clean_sweep(rf::Player& player, int opposing_team)
{
    return player.team == opposing_team &&
    !player.is_non_participant() &&
    !player.is_spectator &&
    !player_is_idle(&player);
}

void check_clean_sweep(rf::Player* killer, rf::Player* victim)
{
    if (!multi_is_team_game_type()) {
        return;
    }
    AwardPlayerState* state = state_for(killer);
    const int victim_id = player_id_of(victim);
    if (!state || victim_id < 0) {
        return;
    }
    state->kills_this_life.set(static_cast<size_t>(victim_id));
    if (state->clean_sweep_granted) {
        return;
    }

    const int opposing_team = killer->team == 0 ? 1 : 0;
    int required = 0;
    for (rf::Player& other : SinglyLinkedList{rf::player_list}) {
        const int other_id = player_id_of(&other);
        if (other_id < 0 || !counts_toward_clean_sweep(other, opposing_team)) {
            continue;
        }
        ++required;
        if (!state->kills_this_life.test(static_cast<size_t>(other_id))) {
            return;
        }
    }
    if (required < clean_sweep_min_roster) {
        return;
    }
    state->clean_sweep_granted = true;
    grant_award(killer, AwardId::clean_sweep, victim);
}

void check_quickdraw(rf::Player* killer, rf::Player* victim, int weapon_type, bool splash)
{
    if (splash || !is_scoped_weapon(weapon_type) || player_is_zoomed(killer)) {
        return;
    }
    rf::Entity* victim_ep = rf::entity_from_handle(victim->entity_handle);
    if (!victim_ep || !is_scoped_weapon(victim_ep->ai.current_primary_weapon)) {
        return;
    }
    if (!player_is_zoomed(victim)) {
        return;
    }
    grant_award(killer, AwardId::quickdraw, victim);
}

void check_stonewall(rf::Player* killer, rf::Player* victim, int victim_team)
{
    if (!player_is_objective_runner(victim, victim_team)) {
        return;
    }
    AwardPlayerState* state = state_for(killer);
    if (!state) {
        return;
    }
    if (++state->carrier_kills >= stonewall_kills_needed) {
        state->carrier_kills = 0;
        grant_award(killer, AwardId::stonewall, victim);
    }
}

void check_last_laugh(rf::Player* killer, rf::Player* victim, int weapon_type, int killer_entity_handle)
{
    rf::Entity* killer_ep = rf::entity_from_handle(killer_entity_handle);
    if (!killer_ep) {
        return;
    }
    // Exactly the state Last Stand excludes.
    if (killer_ep->life > 0.0f && !rf::entity_is_dying(killer_ep)) {
        return;
    }
    const int killer_id = player_id_of(killer);
    const int victim_id = player_id_of(victim);
    const bool own_fire = killer_id >= 0 && victim_id >= 0
        && mutators_player_fire_igniter(static_cast<uint8_t>(victim_id)) == killer_id;
    if (!is_explosive_weapon(weapon_type) && !own_fire) {
        return;
    }
    grant_award(killer, AwardId::last_laugh, victim);
}

void check_depth_charge(rf::Player* killer, rf::Player* victim, int killer_entity_handle)
{
    rf::Entity* killer_ep = rf::entity_from_handle(killer_entity_handle);
    rf::Entity* victim_ep = rf::entity_from_handle(victim->entity_handle);
    if (!killer_ep || !victim_ep) {
        return;
    }
    if (!(killer_ep->entity_flags & rf::EF_IN_WATER) || !(victim_ep->entity_flags & rf::EF_IN_WATER)) {
        return;
    }
    grant_award(killer, AwardId::depth_charge, victim);
}

// The victim's spray stamp survives their death.
void check_censored(rf::Player* killer, rf::Player* victim)
{
    const AwardPlayerState* state = state_for(victim);
    if (!state || state->last_spray_ms == 0) {
        return;
    }
    if (now_ms() - state->last_spray_ms <= censored_window_ms) {
        grant_award(killer, AwardId::censored, victim);
    }
}

// A shotgun volley is many projectiles, so the shot scope cannot group its kills - only their
// arrival time can.
void check_shotgun_volley(rf::Player* killer, rf::Player* victim, int weapon_type, bool splash)
{
    if (splash ||
        !kill_attribution_is_valid_weapon_type(weapon_type) ||
        weapon_type != rf::shotgun_weapon_type) {
        return;
    }
    AwardPlayerState* state = state_for(killer);
    if (!state) {
        return;
    }
    const int64_t now = now_ms();
    const bool same_volley = state->last_shotgun_kill_ms != 0
        && now - state->last_shotgun_kill_ms <= award_repeat_window_ms;
    state->shotgun_volley_kills = same_volley ? state->shotgun_volley_kills + 1 : 1;
    state->last_shotgun_kill_ms = now;
    if (state->shotgun_volley_kills == shotgun_volley_kills_needed) {
        grant_award(killer, AwardId::twofer, victim);
    }
}

void check_x_ray(rf::Player* attacker, rf::Player* victim, int weapon_type)
{
    if (!kill_attribution_is_valid_weapon_type(weapon_type)
        || weapon_type != rf::rail_gun_weapon_type) {
        return;
    }
    AwardPlayerState* state = state_for(attacker);
    if (!state) {
        return;
    }
    const int64_t now = now_ms();
    if (state->last_scanner_ms != 0 && now - state->last_scanner_ms <= x_ray_scanner_lockout_ms) {
        return;
    }
    // Only the Salvage, Scavenger and Bagman carriers are outlined through walls, so exclude them.
    if ((salvage_framework_active() && salvage_get_carrier() == victim) || g_bagman_info.carrier == victim) {
        return;
    }
    rf::Entity* attacker_ep = rf::entity_from_handle(attacker->entity_handle);
    rf::Entity* victim_ep = rf::entity_from_handle(victim->entity_handle);
    if (!attacker_ep || !victim_ep || player_scanner_engaged(attacker, attacker_ep)) {
        return;
    }
    rf::Vector3 p0 = attacker_ep->eye_pos;
    rf::Vector3 p1 = victim_ep->pos;
    rf::GCollisionOutput collision{};
    if (!rf::collide_linesegment_level_solid(p0, p1, award_trace_flags, &collision)) {
        return; // the shot had a clean line, so nothing was shot through
    }
    grant_award(attacker, AwardId::x_ray, victim);
}

// The crush is only ever dealt to a player on foot: a seated rider is exempt from it.
void check_roadkill(rf::Player* killer, rf::Player* victim, const AwardKillVehicles& vehicles)
{
    if (vehicles.roadkill_class >= 0) {
        grant_award(killer, AwardId::roadkill, victim);
    }
}

void check_grand_theft_auto(rf::Player* killer, rf::Player* victim, int killer_entity_handle,
                            const AwardKillVehicles& vehicles)
{
    AwardPlayerState* state = state_for(killer);
    if (!state || state->stolen_hull_handle == -1 || vehicles.kill_class < 0) {
        return;
    }
    const rf::Entity* hull = vehicle_ridden_live_hull(rf::entity_from_handle(killer_entity_handle));
    if (!hull || hull->handle != state->stolen_hull_handle) {
        return;
    }
    state->stolen_hull_handle = -1;
    grant_award(killer, AwardId::grand_theft_auto, victim);
}

void note_vehicle_blast_kill(rf::Player* killer, rf::Player* victim, int blast_hull)
{
    const int killer_id = player_id_of(killer);
    if (blast_hull == -1 || killer_id < 0) {
        return;
    }
    for (auto frame = g_vehicle_blast_stack.rbegin(); frame != g_vehicle_blast_stack.rend(); ++frame) {
        if (frame->hull_handle != blast_hull) {
            continue;
        }
        auto it = std::find_if(frame->killers.begin(), frame->killers.end(),
                               [&](const DetonationKills& k) { return k.killer_id == killer_id; });
        if (it == frame->killers.end()) {
            frame->killers.push_back({static_cast<uint8_t>(killer_id), 1, victim});
        }
        else {
            ++it->count;
            it->last_victim = victim;
        }
        return;
    }
}

// Pruned against the live holds rather than cleared at each powerup's end, so nothing has to watch for
// one running out.
void prune_power_pickups(rf::Player* player, AwardPlayerState& state)
{
    state.power_pickup_mask &= powerup_stats_mask(player);
}

void check_power_trip(rf::Player* killer, rf::Player* victim)
{
    AwardPlayerState* state = state_for(killer);
    if (!state) {
        return;
    }
    prune_power_pickups(killer, *state);
    bool grant = false;
    for (size_t i = 0; i < afstats::powerup_count; ++i) {
        const auto bit = static_cast<uint8_t>(1u << i);
        if (!(state->power_pickup_mask & bit) || (state->power_trip_granted_mask & bit)) {
            continue;
        }
        if (++state->power_trip_kills[i] >= power_trip_kills_needed) {
            state->power_trip_granted_mask |= bit;
            grant = true;
        }
    }
    // Once for the kill, however many of the running pickups it completes.
    if (grant) {
        grant_award(killer, AwardId::power_trip, victim);
    }
}

// Nemesis is a private status message pair, never an award; Revenge is the award for breaking one.
void update_nemesis(rf::Player* killer, rf::Player* victim)
{
    const int killer_id = player_id_of(killer);
    const int victim_id = player_id_of(victim);
    if (killer_id < 0 || victim_id < 0) {
        return;
    }

    NemesisPair& forward = g_nemesis[nemesis_key(static_cast<uint8_t>(killer_id), static_cast<uint8_t>(victim_id))];
    if (forward.count < 0xFF) {
        ++forward.count;
    }
    if (forward.count == nemesis_kills_needed && !forward.active) {
        forward.active = true;
        af_broadcast_automated_chat_msg(
            std::format("{} is dominating {}!", killer->name.c_str(), victim->name.c_str()));
        // In addition to the broadcast, not instead of it. The `active` latch is what keeps this to
        // once per relationship: it only clears when the pair is erased (revenge, leave, level
        // end), so a re-established domination can grant again.
        grant_award(killer, AwardId::dominating, victim);
    }

    const uint16_t reverse_key = nemesis_key(static_cast<uint8_t>(victim_id), static_cast<uint8_t>(killer_id));
    auto reverse_it = g_nemesis.find(reverse_key);
    if (reverse_it == g_nemesis.end()) {
        return;
    }
    if (reverse_it->second.active) {
        g_nemesis.erase(reverse_it);
        af_broadcast_automated_chat_msg(
            std::format("{} got revenge on {}!", killer->name.c_str(), victim->name.c_str()));
        grant_award(killer, AwardId::revenge, victim);
    }
    else {
        reverse_it->second.count = 0;
    }
}

void clear_nemesis_pairs_for(uint8_t player_id)
{
    for (auto it = g_nemesis.begin(); it != g_nemesis.end();) {
        const uint8_t killer_id = static_cast<uint8_t>(it->first >> 8);
        const uint8_t victim_id = static_cast<uint8_t>(it->first & 0xFF);
        if (killer_id == player_id || victim_id == player_id) {
            it = g_nemesis.erase(it);
        }
        else {
            ++it;
        }
    }
}

// -------------------------------------------------------------------------
// Client display
// -------------------------------------------------------------------------

struct AwardDisplay
{
    // Title for listings, e.g. the demo seek bar's award markers
    const char* name;
    const char* text;
    // std::format pattern naming the opposing player, for the awards whose callout does. Null for
    // the rest, and unused when the id did not resolve to a connected player.
    const char* victim_text;
};

constexpr std::array<AwardDisplay, award_id_count> award_display{{
    {"Toasty", "TOASTY!", nullptr},
    {"Massacre", "MASSACRE!", nullptr},
    {"Riot Control", "RIOT CONTROL!", nullptr},
    {"2fer", "2FER!", nullptr},
    {"Impressive", "IMPRESSIVE!", nullptr},
    {"Excellent", "EXCELLENT!", nullptr},
    {"Overkill", "OVERKILL!", nullptr},
    {"Unstoppable", "UNSTOPPABLE!", nullptr},
    {"Last Stand", "LAST STAND!", nullptr},
    {"Flag Runner", "FLAG RUNNER!", nullptr},
    {"Capture Denied", "CAPTURE DENIED!", nullptr},
    {"Area Denied", "AREA DENIED!", nullptr},
    {"Bag Check", "BAG CHECK!", nullptr},
    {"Revenge", "REVENGE!", "REVENGE ON {}!"},
    {"Dominating", "DOMINATING!", "DOMINATING {}!"},
    {"Rampage", "YOU'RE ON A RAMPAGE!", nullptr},
    {"First Blood", "FIRST BLOOD!", nullptr},
    {"Hat Trick", "HAT TRICK!", nullptr},
    {"Airshot", "AIRSHOT!", nullptr},
    {"Clean Sweep", "CLEAN SWEEP!", nullptr},
    {"Quickdraw", "QUICKDRAW!", nullptr},
    {"Stonewall", "STONEWALL!", nullptr},
    {"Lockdown", "LOCKDOWN!", nullptr},
    {"Hazard Pay", "HAZARD PAY!", nullptr},
    {"Last Laugh", "LAST LAUGH!", nullptr},
    {"X-Ray", "X-RAY!", nullptr},
    {"Depth Charge", "DEPTH CHARGE!", nullptr},
    {"Censored", "CENSORED!", nullptr},
    {"Special Delivery", "SPECIAL DELIVERY!", nullptr},
    {"Roadkill", "ROADKILL!", nullptr},
    {"Scrap Metal", "SCRAP METAL!", nullptr},
    {"Wrecking Crew", "WRECKING CREW!", nullptr},
    {"Grand Theft Auto", "GRAND THEFT AUTO!", nullptr},
    {"Air Superiority", "AIR SUPERIORITY!", nullptr},
    {"Power Trip", "POWER TRIP!", nullptr},
    {"Stacked", "STACKED!", nullptr},
    {"Mechanic", "MECHANIC!", nullptr},
}};
static_assert(award_display[award_id_count - 1].text != nullptr);

constexpr int award_display_seconds = 3;
// The notification slot fades for 500 ms after its 3 s, so the next award waits for both.
constexpr int award_slot_hold_ms = award_display_seconds * 1000 + 500;
// A burst of awards is worth showing; an unbounded backlog is not.
constexpr size_t award_queue_cap = 8;

// Excellent and Impressive are the common ones, suppress if they overlap.
bool is_low_priority_award(uint8_t award_id)
{
    return award_id == static_cast<uint8_t>(AwardId::excellent)
        || award_id == static_cast<uint8_t>(AwardId::impressive);
}

// A low-priority award may not display until this long after it arrives, which is what makes
// yielding possible at all.
constexpr int award_low_priority_hold_ms = 250;

// The text is resolved when the award arrives, not when it reaches the front of the queue: a named
// player may well have left by then.
struct QueuedAward
{
    std::string text;
    // Set only for low-priority awards, and the marker for them: valid() means "this entry is
    // still yielding", and it may not be displayed before it elapses.
    rf::Timestamp hold_until;
};

std::deque<QueuedAward> g_award_queue;
rf::Timestamp g_award_slot_busy;

// True while an award this module put up still owns the big slot. Same test awards_client_do_frame
// gates on, so "already displaying" means the same thing to both.
bool award_currently_displaying()
{
    return hud_big_notification_current_type() == HudNotificationType::Award
        && g_award_slot_busy.valid() && !g_award_slot_busy.elapsed();
}

ConsoleCommand2 awards_display_cmd{
    "cl_awards",
    []() {
        setting_console_set("show_awards", "toggle");
        rf::console::print("Display of earned awards is {}",
                           g_alpine_game_config.show_awards ? "enabled" : "disabled");
    },
    "Toggle whether to display awards you earn in multiplayer",
    "cl_awards",
};

ConsoleCommand2 domination_msgs_display_cmd{
    "cl_dominationmsgs",
    []() {
        setting_console_set("show_domination_msgs", "toggle");
        rf::console::print("Display of dominating and revenge chat messages is {}",
                           g_alpine_game_config.show_domination_msgs ? "enabled" : "disabled");
    },
    "Toggle whether to display dominating and revenge server messages in chat",
    "cl_dominationmsgs",
};

} // namespace

const char* award_name(uint8_t award_id)
{
    return award_id < award_id_count ? award_display[award_id].name : nullptr;
}

// -------------------------------------------------------------------------
// Server entry points
// -------------------------------------------------------------------------

void awards_on_kill(rf::Player* victim, rf::Player* killer, int weapon_type, bool splash,
                    int killer_entity_handle, int victim_team, float damage, float victim_life,
                    float victim_armor, const AwardKillVehicles& vehicles)
{
    if (!awards_tracking_active() || !victim) {
        return;
    }

    // Victim-side resets run for every death, including suicides and world deaths.
    if (AwardPlayerState* victim_state = state_for(victim)) {
        victim_state->streak = 0;
        victim_state->hit_chain = 0;
        victim_state->shot_pending = false;
        victim_state->clean_steal = false;
        victim_state->caps_this_life = 0;
        victim_state->carrier_kills = 0;
        victim_state->kills_this_life.reset();
        victim_state->clean_sweep_granted = false;
        victim_state->riot_stick_kill = false;
        victim_state->riot_shield_kill = false;
        victim_state->stolen_hull_handle = -1;
        victim_state->power_pickup_mask = 0;
        victim_state->repaired_this_life = 0.0f;
    }

    sample_ctf_flag_homes();

    if (!is_enemy_kill(killer, victim, victim_team)) {
        return;
    }

    // Massacre: bank the kill against the detonation currently being resolved. The detonation's
    // own direct-hit victim counts alongside the splash victims, but only for a weapon that
    // actually explodes - a piercing bullet's kills are the 2fer's business, not this one's.
    if (kill_attribution_in_splash_scope() && g_detonation_depth > 0) {
        const DetonationFrame& open_frame = g_detonation_stack[g_detonation_depth - 1];
        const bool direct_of_this_detonation =
            !splash && weapon_type >= 0 && weapon_type == open_frame.weapon_type
            && kill_attribution_is_valid_weapon_type(weapon_type)
            && rf::weapon_types[weapon_type].damage_radius > 0.0f;
        const int killer_id = (splash || direct_of_this_detonation) ? player_id_of(killer) : -1;
        if (killer_id >= 0) {
            DetonationFrame& frame = g_detonation_stack[g_detonation_depth - 1];
            auto it = std::find_if(frame.killers.begin(), frame.killers.end(),
                                   [&](const DetonationKills& k) { return k.killer_id == killer_id; });
            if (it == frame.killers.end()) {
                frame.killers.push_back({static_cast<uint8_t>(killer_id), 1, victim});
            }
            else {
                ++it->count;
                it->last_victim = victim;
            }
        }
    }

    // 2fer: kills of one lag-compensated sniper/rail bolt. Only kills this bolt itself dealt
    // count - a kill credited to the same shooter by another source while the scope is open
    // (their burning victim expiring, a rocket already in the air) carries a different weapon
    // type and is no part of the shot.
    if (!splash && g_shot_scope.active && g_shot_scope.shooter_id == player_id_of(killer)
        && weapon_type == g_shot_scope.weapon_type && is_sniper_or_rail(g_shot_scope.weapon_type)) {
        ++g_shot_scope.enemy_kills;
        g_shot_scope.last_victim = victim;
    }

    note_vehicle_blast_kill(killer, victim, vehicles.blast_hull);

    // Toasty: burn out the player who lit you, before their fire burns you out. Both ids must be
    // real - the accessor's "not burning" answer is -1, which is also an unidentifiable player.
    const int killer_id = player_id_of(killer);
    const int toasty_victim_id = player_id_of(victim);
    if (killer_id >= 0 && toasty_victim_id >= 0
        && mutators_player_fire_igniter(static_cast<uint8_t>(killer_id)) == toasty_victim_id) {
        grant_award(killer, AwardId::toasty, victim);
    }

    if (!g_first_blood_claimed) {
        g_first_blood_claimed = true;
        grant_award(killer, AwardId::first_blood, victim);
    }

    check_censored(killer, victim);
    check_riot_control(killer, victim, weapon_type);
    check_excellent(killer, victim);
    check_streak(killer, victim);
    check_overkill(killer, victim, damage, victim_life, victim_armor);
    check_last_stand(killer, victim, killer_entity_handle);
    check_capture_denied(killer, victim, victim_team);
    check_area_denied(killer, victim, victim_team);
    check_airshot(killer, victim, weapon_type, splash);
    check_clean_sweep(killer, victim);
    check_quickdraw(killer, victim, weapon_type, splash);
    check_stonewall(killer, victim, victim_team);
    check_last_laugh(killer, victim, weapon_type, killer_entity_handle);
    check_depth_charge(killer, victim, killer_entity_handle);
    check_shotgun_volley(killer, victim, weapon_type, splash);
    check_roadkill(killer, victim, vehicles);
    check_grand_theft_auto(killer, victim, killer_entity_handle, vehicles);
    check_power_trip(killer, victim);
    update_nemesis(killer, victim);
}

void awards_on_weapon_fired(rf::Player* player, int weapon_type)
{
    if (!awards_tracking_active()) {
        return;
    }
    AwardPlayerState* state = state_for(player);
    if (!state) {
        return;
    }
    if (!is_sniper_or_rail(weapon_type)) {
        // Any other shot ends the chain: Impressive is consecutive sniper/rail hits, nothing else.
        state->shot_pending = false;
        state->hit_chain = 0;
        return;
    }
    if (state->shot_pending) {
        state->hit_chain = 0; // the previous bolt never landed
    }
    state->shot_pending = true;
}

void awards_on_direct_hit(rf::Player* attacker, rf::Player* victim, int weapon_type)
{
    if (!awards_tracking_active() || !victim || !is_sniper_or_rail(weapon_type)) {
        return;
    }
    AwardPlayerState* state = state_for(attacker);
    if (!state) {
        return;
    }
    state->shot_pending = false;
    if (!is_enemy_kill(attacker, victim, victim->team)) {
        state->hit_chain = 0; // a teammate is a miss as far as this chain is concerned
        return;
    }
    ++state->hit_chain;
    if (state->hit_chain % 2 == 0) {
        grant_award(attacker, AwardId::impressive, victim);
    }
    check_x_ray(attacker, victim, weapon_type);
}

void awards_server_do_frame()
{
    if (!awards_tracking_active()) {
        return;
    }
    const int64_t now = now_ms();
    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        AwardPlayerState* state = state_for(&player);
        if (!state) {
            continue;
        }
        if (player_scanner_engaged(&player, rf::entity_from_handle(player.entity_handle))) {
            state->last_scanner_ms = now;
        }
    }
}

AwardsShotScope::AwardsShotScope(rf::Entity* shooter, rf::Weapon* wp)
{
    if (!awards_tracking_active() || !shooter || !wp) {
        return;
    }
    rf::Player* shooter_player = rf::player_from_entity_handle(shooter->handle);
    const int shooter_id = player_id_of(shooter_player);
    if (shooter_id < 0) {
        return;
    }
    active_ = true;
    g_shot_scope = {true, shooter_id, wp->info_index, 0, nullptr};
}

AwardsShotScope::~AwardsShotScope()
{
    if (!active_) {
        return;
    }
    const ShotScopeState finished = g_shot_scope;
    // These do not nest in practice, but clearing rather than restoring keeps a stray nested scope
    // from being credited with the outer shot's kills.
    g_shot_scope = {};
    if (finished.enemy_kills >= 2) {
        grant_award(rf::multi_find_player_by_id(static_cast<uint8_t>(finished.shooter_id)),
                    AwardId::twofer, finished.last_victim);
    }
}

void awards_detonation_begin(int weapon_type)
{
    ++g_detonation_depth;
    if (static_cast<int>(g_detonation_stack.size()) < g_detonation_depth) {
        g_detonation_stack.resize(g_detonation_depth);
    }
    g_detonation_stack[g_detonation_depth - 1].weapon_type = weapon_type;
    g_detonation_stack[g_detonation_depth - 1].killers.clear();
}

void awards_detonation_end()
{
    if (g_detonation_depth <= 0) {
        return;
    }
    // Read out before leaving the frame: granting is not allowed to be looking at stack storage a
    // later scope could reuse.
    std::vector<DetonationKills> winners;
    for (const DetonationKills& entry : g_detonation_stack[g_detonation_depth - 1].killers) {
        if (entry.count >= massacre_kills_needed) {
            winners.push_back(entry);
        }
    }
    --g_detonation_depth;
    for (const DetonationKills& entry : winners) {
        grant_award(rf::multi_find_player_by_id(entry.killer_id), AwardId::massacre, entry.last_victim);
    }
}

AwardsVehicleBlastScope::AwardsVehicleBlastScope(const rf::Entity* hull, rf::Player* destroyer,
                                                 int destroyer_vehicle_class)
{
    if (!awards_tracking_active() || !hull) {
        return;
    }
    int enemy_occupants = 0;
    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        const rf::Entity* occupant = rf::entity_from_handle(player.entity_handle);
        if (occupant && occupant->host_handle == hull->handle && is_enemy_kill(destroyer, &player, player.team)) {
            ++enemy_occupants;
        }
    }
    active_ = true;
    g_vehicle_blast_stack.push_back(
        {hull->handle, vehicle_damage_class(hull), destroyer, destroyer_vehicle_class, enemy_occupants, {}});
}

AwardsVehicleBlastScope::~AwardsVehicleBlastScope()
{
    if (!active_ || g_vehicle_blast_stack.empty()) {
        return;
    }
    const VehicleBlastFrame frame = std::move(g_vehicle_blast_stack.back());
    g_vehicle_blast_stack.pop_back();

    const int destroyer_id = player_id_of(frame.destroyer);
    rf::Player* destroyer_victim = nullptr;
    for (const DetonationKills& entry : frame.killers) {
        if (entry.killer_id == destroyer_id) {
            destroyer_victim = entry.last_victim;
        }
        if (entry.count >= wrecking_crew_kills_needed) {
            grant_award(rf::multi_find_player_by_id(entry.killer_id), AwardId::wrecking_crew, entry.last_victim);
        }
    }
    if (!frame.destroyer || frame.enemy_occupants <= 0) {
        return;
    }
    if (frame.destroyer_vehicle_class < 0) {
        grant_award(frame.destroyer, AwardId::scrap_metal, destroyer_victim);
    }
    else if (frame.hull_class == VDC_FIGHTER && frame.destroyer_vehicle_class == VDC_FIGHTER) {
        grant_award(frame.destroyer, AwardId::air_superiority, destroyer_victim);
    }
}

void awards_on_vehicle_claimed(const rf::Player* player, int hull_handle)
{
    if (!awards_tracking_active()) {
        return;
    }
    if (AwardPlayerState* state = state_for(player)) {
        state->stolen_hull_handle = hull_handle;
    }
}

void awards_on_powerup_pickup(rf::Player* player, afstats::Powerup powerup)
{
    const auto index = static_cast<size_t>(powerup);
    if (!awards_tracking_active() || index >= afstats::powerup_count) {
        return;
    }
    AwardPlayerState* state = state_for(player);
    if (!state) {
        return;
    }
    // Before this pickup's own bit: a powerup that has run out must not be one of Stacked's three.
    prune_power_pickups(player, *state);
    const auto bit = static_cast<uint8_t>(1u << index);
    const bool already_running = (state->power_pickup_mask & bit) != 0;
    state->power_pickup_mask |= bit;
    state->power_trip_kills[index] = 0;
    state->power_trip_granted_mask &= static_cast<uint8_t>(~bit);
    if (!already_running && std::popcount(state->power_pickup_mask) == stacked_powerups_needed) {
        grant_award(player, AwardId::stacked);
    }
}

void awards_on_vehicle_repair(rf::Player* player, float repaired)
{
    if (!awards_tracking_active()) {
        return;
    }
    AwardPlayerState* state = state_for(player);
    if (!state) {
        return;
    }
    state->repaired_this_life += repaired;
    if (state->repaired_this_life >= mechanic_repair_needed) {
        state->repaired_this_life -= mechanic_repair_needed;
        grant_award(player, AwardId::mechanic);
    }
}

void awards_on_ctf_flag_taken(rf::Player* player, bool red_flag, bool from_base, const rf::Vector3& pos)
{
    if (!awards_tracking_active()) {
        return;
    }
    if (from_base) {
        // A flag sitting at home is on its stand, so this is the one position that is always the
        // stand even after the flag leaves it.
        note_flag_home(red_flag ? 0 : 1, pos);
    }
    if (AwardPlayerState* state = state_for(player)) {
        // Only a steal starts a run; picking one off the ground does not.
        state->clean_steal = from_base;
    }
}

void awards_on_ctf_flag_dropped(rf::Player* player)
{
    if (AwardPlayerState* state = state_for(player)) {
        state->clean_steal = false;
    }
}

// Hat Trick, shared by the modes that capture an objective (CTF, SAL and SCAV).
static void awards_note_capture(rf::Player* player)
{
    AwardPlayerState* state = state_for(player);
    if (!state) {
        return;
    }
    if (++state->caps_this_life >= hat_trick_caps_needed) {
        state->caps_this_life = 0;
        grant_award(player, AwardId::hat_trick);
    }
}

void awards_on_ctf_capture(rf::Player* player)
{
    if (!awards_tracking_active()) {
        return;
    }
    AwardPlayerState* state = state_for(player);
    if (state && state->clean_steal) {
        state->clean_steal = false;
        grant_award(player, AwardId::flag_runner);
    }
    awards_note_capture(player);
}

void awards_on_sal_flag_taken(rf::Player* player, bool from_spawn)
{
    // The shared objective engine also runs Scavenger, which has no Flag Runner.
    if (!gt_is_salvage() || !awards_tracking_active()) {
        return;
    }
    if (AwardPlayerState* state = state_for(player)) {
        state->clean_steal = from_spawn;
    }
}

void awards_on_sal_flag_dropped(rf::Player* player)
{
    if (!gt_is_salvage()) {
        return;
    }
    if (AwardPlayerState* state = state_for(player)) {
        state->clean_steal = false;
    }
}

void awards_on_sal_capture(rf::Player* player)
{
    if (!awards_tracking_active()) {
        return;
    }
    AwardPlayerState* state = state_for(player);
    if (gt_is_salvage() && state && state->clean_steal) {
        state->clean_steal = false;
        grant_award(player, AwardId::flag_runner);
    }
    awards_note_capture(player);
}

void awards_on_bagman_pickup(rf::Player* player)
{
    if (AwardPlayerState* state = state_for(player)) {
        state->has_bag_pickup = true;
        state->bag_pickup_ms = now_ms();
        state->hazard_pay_granted = false;
    }
}

void awards_on_bagman_drop(rf::Player* player)
{
    if (AwardPlayerState* state = state_for(player)) {
        state->has_bag_pickup = false;
        state->hazard_pay_granted = false;
    }
}

void awards_on_bagman_hold_tick(rf::Player* carrier)
{
    if (!awards_tracking_active()) {
        return;
    }
    AwardPlayerState* state = state_for(carrier);
    if (!state || !state->has_bag_pickup || state->hazard_pay_granted) {
        return;
    }
    if (now_ms() - state->bag_pickup_ms < hazard_pay_hold_ms) {
        return;
    }
    state->hazard_pay_granted = true;
    grant_award(carrier, AwardId::hazard_pay);
}

void awards_on_spray(rf::Player* player)
{
    if (!awards_tracking_active()) {
        return;
    }
    if (AwardPlayerState* state = state_for(player)) {
        state->last_spray_ms = now_ms();
    }
}

void awards_on_bagman_carrier_death(rf::Player* carrier, rf::Player* killer)
{
    if (!awards_tracking_active() || !carrier || !killer || killer == carrier) {
        return;
    }
    if (gt_is_tbag() && killer->team == carrier->team) {
        return;
    }
    const AwardPlayerState* state = state_for(carrier);
    if (!state || !state->has_bag_pickup) {
        return;
    }
    if (now_ms() - state->bag_pickup_ms <= bag_check_window_ms) {
        grant_award(killer, AwardId::bag_check, carrier);
    }
}

void awards_on_player_destroy(rf::Player* player)
{
    const int id = player_id_of(player);
    if (id < 0) {
        return;
    }
    player_states()[id] = AwardPlayerState{};
    // The id is about to be handed to somebody else.
    for (AwardPlayerState& state : player_states()) {
        state.kills_this_life.reset(static_cast<size_t>(id));
    }
    clear_nemesis_pairs_for(static_cast<uint8_t>(id));
}

void awards_level_init()
{
    std::fill(player_states().begin(), player_states().end(), AwardPlayerState{});
    g_first_blood_claimed = false;
    g_nemesis.clear();
    g_detonation_stack.clear();
    g_detonation_depth = 0;
    g_vehicle_blast_stack.clear();
    g_shot_scope = {};
    g_flag_home[0] = {};
    g_flag_home[1] = {};
    g_award_queue.clear();
    g_award_slot_busy.invalidate();
}

// -------------------------------------------------------------------------
// Client entry points
// -------------------------------------------------------------------------

void awards_client_on_award_received(uint8_t award_id, uint8_t victim_player_id)
{
    if (award_id >= award_id_count) {
        return; // fail closed on anything this build does not know
    }
    if (!g_alpine_game_config.show_awards || rf::is_dedicated_server) {
        return;
    }

    // Demotion of excellent/impressive, both halves of it. A low-priority entry is therefore only
    // ever alone in the queue: anything already there or on screen drops it on arrival, and
    // anything arriving later evicts it.
    const bool low_priority = is_low_priority_award(award_id);
    if (low_priority) {
        if (!g_award_queue.empty() || award_currently_displaying()) {
            return; // something better is already showing or waiting - this one is not worth a turn
        }
    }
    else if (!g_award_queue.empty() && g_award_queue.front().hold_until.valid()) {
        // The queued low-priority award has not displayed yet (entries are popped when they do),
        // so this is the "same time as another award" case: the better award takes the slot.
        g_award_queue.clear();
    }

    if (g_award_queue.size() >= award_queue_cap) {
        // Keep the newest: with a full backlog the oldest entries are the stalest news.
        g_award_queue.pop_front();
    }

    const AwardDisplay& display = award_display[award_id];
    std::string text = display.text;
    if (display.victim_text && victim_player_id != award_no_victim) {
        // An unresolvable id (the named player left in the interim) falls back to the plain text
        // rather than dropping the award.
        if (rf::Player* victim = rf::multi_find_player_by_id(victim_player_id)) {
            const char* victim_name = victim->name.c_str();
            text = std::vformat(display.victim_text, std::make_format_args(victim_name));
        }
    }

    QueuedAward queued{std::move(text), {}};
    if (low_priority) {
        queued.hold_until.set(award_low_priority_hold_ms);
    }
    g_award_queue.push_back(std::move(queued));
}

void awards_client_reset()
{
    g_award_queue.clear();
    g_award_slot_busy.invalidate();
    hud_notification_remove(HudNotificationType::Award, true);
}

// Matches the chat lines update_nemesis broadcasts.
bool awards_is_domination_chat_msg(std::string_view msg)
{
    return string_ends_with(msg, "!")
        && (string_contains(msg, " is dominating ") || string_contains(msg, " got revenge on "));
}

void awards_client_do_frame()
{
    if (!rf::is_multi) {
        // Left the server: whatever is still queued belongs to the game it was earned in, and
        // must not fire over the menu.
        g_award_queue.clear();
        g_award_slot_busy.invalidate();
        return;
    }
    if (demo_playback_sim_frozen()) {
        return;
    }
    if (g_award_queue.empty()) {
        return;
    }
    if (g_award_queue.front().hold_until.valid() && !g_award_queue.front().hold_until.elapsed()) {
        // A low-priority award still inside its yield window. Nothing can be stuck behind it: it is
        // only ever queued when the queue is empty, and anything arriving after it removes it.
        return;
    }

    // The queue advances off the slot's real state, not just its own timer: another notification
    // may have overwritten the award (no point waiting out a callout nobody can see), and one it
    // did not put up itself is not its to stomp.
    const HudNotificationType big_slot_type = hud_big_notification_current_type();
    if (big_slot_type == HudNotificationType::Award) {
        if (g_award_slot_busy.valid() && !g_award_slot_busy.elapsed()) {
            return; // the previous award still owns the slot
        }
    }
    else if (big_slot_type != HudNotificationType::None) {
        return;
    }

    QueuedAward award = std::move(g_award_queue.front());
    g_award_queue.pop_front();

    hud_notification_show(std::move(award.text), award_display_seconds, HudNotificationType::Award, true);
    const int sound_id = get_award_sound_id();
    if (sound_id >= 0) {
        play_local_sound_2d(static_cast<uint16_t>(sound_id), 0, 1.0f);
    }
    g_award_slot_busy.set(award_slot_hold_ms);
}

void awards_do_patch()
{
    awards_display_cmd.register_cmd();
    domination_msgs_display_cmd.register_cmd();
}
