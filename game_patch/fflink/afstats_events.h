#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rf
{
    struct Player;
    struct Vector3;
    struct Clutter;
}

// Server-side event reporting to FactionFiles.
// Layer 1 (this header) is the emission API gameplay hooks call. Every entry
// point is a no-op unless this process is a multiplayer server with FactionFiles
// stats enabled, so call sites never need to gate themselves. Events may queue
// before the GSK -> GSSK exchange completes; nothing transmits until a GSSK
// exists.
namespace afstats {

// Wire values from the game_end `end_type` registry.
enum class GameEndType : uint8_t
{
    time_limit = 0,
    score_limit = 1,
    map_change_manual = 2,
    map_change_vote = 3,
    level_logic = 4,
    server_shutdown = 5,
};

// Wire values from the player_leave `reason` registry.
enum class LeaveReason : uint8_t
{
    quit = 0,
    kicked = 1,
    banned = 2,
    timeout = 3,
    vote_kicked = 4,
    bot_removed = 5, // the server's bot handling took the bot out
};

// Wire values from the player_join `bot_source` registry.
enum class BotSource : uint8_t
{
    client = 0,    // a bot client: a connected client playing as a bot
    config = 1,    // the server config's server_bots
    join_bots = 2, // the Join Bots mutator
    command = 3,   // sv_bot_add
};

// Wire values from the status `kind` registry.
enum class StatusKind : uint8_t
{
    team_red = 0,
    team_blue = 1,
    spec_start = 2,
    spec_stop = 3,
    idle_start = 4,
    idle_stop = 5,
    participate_start = 6,
    participate_stop = 7,
    handicap = 8, // carries the new percent as `value`
};

// Wire values from the flag_event `kind` registry.
enum class FlagEventKind : uint8_t
{
    steal = 0,
    pickup = 1,
    capture = 2,
    drop_death = 3,
    drop_manual = 4,
    return_touch = 5,
    return_timeout = 6,
    reset = 7,
};

// Wire values from the point_event `kind` registry.
enum class PointEventKind : uint8_t
{
    owner_change = 0,
    contest_start = 1,
    contest_end = 2,
    lock_change = 3,
};

// Wire values from the bagman_event `kind` registry.
enum class BagmanEventKind : uint8_t
{
    available = 0,
    pickup = 1,
    drop = 2,
    ret = 3, // "return" on the wire; the keyword is taken
};

// bagman_event `from`, pickup only.
enum class BagmanFrom : uint8_t
{
    spawn = 0,
    dropped = 1,
};

// Wire values from the payload_event `kind` registry.
enum class PayloadEventKind : uint8_t
{
    setup_end = 0,
    rollback_start = 1,
    final_stretch = 2,
    overtime_start = 3,
    overtime_cleared = 4,
    finale = 5,
    stopped = 6,
};

// Wire values from the scav_zone `cause` registry.
enum class ScavZoneCause : uint8_t
{
    initial = 0,
    capture = 1,
    timeout = 2,
};

// Wire values from the vehicle_event `kind` registry.
enum class VehicleEventKind : uint8_t
{
    spawn = 0,
    destroyed = 1,
    claimed = 2,  // a boarder took a hull of another team, or of none, for his own
    returned = 3, // scuttled with no credited destroyer
};

// Wire values from the vehicle_event `cause` registry: what ended a hull.
enum class VehicleEndCause : uint8_t
{
    weapon = 0, // any blow that is not one of the others
    crash = 1,  // a crash, a scrape, a ram or a drill
    hazard = 2, // drowned, or a sub stranded out of the water
    void_fall = 3,
    idle_return = 4,
};

// Wire values from the vehicle_seat `reason` registry. Boarding and moving seats are always voluntary.
enum class VehicleSeatReason : uint8_t
{
    voluntary = 0,
    hull_destroyed = 1,
    disconnect = 2,
    orphan_release = 3, // the server's reconcile freed a seat whose rider or hull was gone
    rider_died = 4,
};

// Wire values from the kill `cause` registry: a death the other kill fields can't tell apart.
enum class KillCause : uint8_t
{
    none = 0, // not sent
    jetpack_explode = 1,
    payload_finale = 2,
};

// Wire values from the match_end `result` registry.
enum class MatchResult : uint8_t
{
    completed = 0,
    canceled = 1,
};

// Wire values from the round_end `result` registry.
enum class RoundResult : uint8_t
{
    completed = 0, // decided (a winner_team or winner_player is set)
    draw = 1,      // ran to completion with no winner (double wipe, timeout tie, mutual death)
    canceled = 2,  // ended without a decided contest (level change, or abandoned before it could start)
};

// Wire values from the game_start `match_state` registry. Stamped on every
// game_start so warmup play is distinguishable from match play at game
// granularity, without depending on the (gap-lossy) match_start/match_end pair.
enum class MatchState : uint8_t
{
    none = 0,
    pre_match = 1,
    match_active = 2,
};

// The `team` registry. 0 and 1 are the engine's own values.
constexpr uint8_t team_red = 0;
constexpr uint8_t team_blue = 1;
constexpr uint8_t team_none = 2;

// Server-side join is complete and the player's identity fields are populated.
// Mints the player's UPSSK and emits `player_join`. Browsers are ignored; bots
// are reported with kind "bot".
void on_player_join(rf::Player* player);

// The client delivered its real PSSK. Emits `player_remediate` and swaps the
// player's stats key when the delivered key differs from the current one.
void on_pssk_received(rf::Player* player);

// Record why a player is about to leave. First writer wins, so tag the specific
// reason before the generic kick path runs.
void note_leave_reason(rf::Player* player, LeaveReason reason);

// The player is being torn down. Must run while PlayerAdditionalData is still
// alive; emits `player_leave` with the partial-game summary.
void on_player_leave(rf::Player* player);

// The server processed a name change for this player. Emits `player_rename` with
// the finalized name; call after the stock handler has applied it. `prev_name` is the
// name the player held before this change (what the stream last reported): an unchanged
// name emits nothing, and successive changes are rate-floored.
void on_player_rename(rf::Player* player, const char* name, std::string_view prev_name);

// A level finished loading server-side. Emits `game_start`.
void on_game_start();

// The session id and per-session game counter the server is currently reporting
// under. The demo recorder stamps these onto an auto-recorded segment's header so the
// segment can be attributed to the same game its events were reported under.
struct GameIdentity
{
    std::string session_id; // 16 lowercase hex
    uint32_t game = 0;      // 1-based, non-zero
};

// Returns the current reporting identity ONLY while afstats is actively reporting to
// FactionFiles for a real open game (a usable GSK is configured and not hard-rejected,
// a game is open, the session id is minted, and the game counter is >= 1); nullopt in
// every other case. Main-thread only: it reads main-thread-owned state without locking.
std::optional<GameIdentity> current_reporting_game();

// Record why the current game is ending. First writer wins; consumed and reset
// by on_game_end(). Without a call the game is reported as a manual change.
void note_game_end_type(GameEndType end_type);

// The server entered limbo, i.e. the game is over. Emits `game_end`.
void on_game_end();

// A lethal blow landed. Arguments mirror what the af_kill_info path already
// computed: `weapon_type` < 0 and a null `killer` are reported as JSON null,
// `kill_flags` is the af_kill_info_flags bitfield and goes on the wire verbatim,
// `assist_player_ids` is the attribution list in most-recent-first order, and
// `vehicle_class` is the wire-frozen VehicleDamageClass of the killing vehicle, or -1
// (reported as JSON null). `victim_vehicle_class` is the same for the hull the victim rode, and
// `killer_seat` the seat of the killing vehicle the blow came from; -1 is null for both. `crit` is
// only reported while the Crits mutator is on.
void on_kill(rf::Player* victim, rf::Player* killer, int weapon_type, int damage_type,
             uint8_t kill_flags, const std::vector<uint8_t>& assist_player_ids,
             const rf::Vector3& victim_pos, const rf::Vector3* killer_pos, int vehicle_class,
             int victim_vehicle_class = -1, int killer_seat = -1, bool crit = false,
             KillCause cause = KillCause::none);

// A player earned an award. `award_id` is the wire-frozen AwardId registry in multi/awards.h and
// goes on the wire verbatim; `victim` is the opposing player the award was earned against, null
// for the awards that have none.
void on_award(rf::Player* player, uint8_t award_id, rf::Player* victim);

// A player spawned server-side.
void on_spawn(rf::Player* player, const rf::Vector3& pos);

// Whether acc/eff info is recorded in full or only weapon bucket.
enum class CountScope
{
    full,
    bucket_only,
};

// Damage was applied to a player. Feeds the windowed `damage` aggregate, and the `hit` /
// `hit_legs` / `hit_torso` / `hit_head` side of the `accuracy` aggregate when `accuracy_hit` is
// set. That flag is the caller's decision that this application counts as an accuracy hit - it
// covers splash detonations and continuous windows too, not just direct projectile impacts.
// `region` is the victim's hit region: -1 = none, 0 = legs, 1 = torso, 2 = head. It is only read
// inside the accuracy-hit branch, and anything other than 0/1/2 increments no region counter.
// A null `attacker` is environmental.
// `amount` is what the hit actually removed from the victim's combined health and armor pools
// capped at what they had left; only a hit that removed nothing (a corpse) is zero.
// `vehicle_class` is the VehicleDamageClass whose weapon or hull dealt the damage, -1 for none.
// `crit` is whether the Crits mutator multiplied this hit.
void on_damage(rf::Player* attacker, rf::Player* victim, int weapon_type, int damage_type,
               float amount, bool accuracy_hit, int region,
               CountScope hit_scope = CountScope::full, int vehicle_class = -1, bool crit = false);

// A player's shot scored its accuracy hit on a vehicle hull with players aboard. A hull has no
// player to take the damage, so this is the hit side of on_damage alone.
void on_hull_hit(rf::Player* attacker, int weapon_type, CountScope hit_scope, int vehicle_class);

// A player pulled a trigger. `projectiles` is the shot's projectile count from the
// weapon table, so pellet weapons account exactly.
// `potential_damage` is what those shots could have dealt at multiplayer damage values (torso),
// the denominator of efficiency. It follows the same scope as the shot it belongs to.
// `vehicle_class` is the VehicleDamageClass of the vehicle that fired, -1 for infantry.
void on_weapon_fired(rf::Player* player, int weapon_type, uint32_t projectiles,
                     CountScope scope = CountScope::full, float potential_damage = 0.0f,
                     int vehicle_class = -1);

// Effective damage this weapon dealt to ANOTHER player - the efficiency numerator. Self-damage is
// never dealt, though its shot's potential still counts, so a rocket jump lowers efficiency.
void on_damage_dealt(rf::Player* attacker, int weapon_type, float amount,
                     CountScope scope = CountScope::full, int vehicle_class = -1);

// Out-of-band player state change. `value` is only read for
// StatusKind::handicap.
void on_status(rf::Player* player, StatusKind kind, int value = 0);

// The server accepted a pong that measured a fresh round-trip. Drives the per-window
// pong counter and min/max; a window with none reports its latencies as -1.
void on_pong_received(rf::Player* player, int rtt_ms);

// A player picked an item up server-side. `respawn_ms` is 0 when it never respawns.
void on_item_pickup(rf::Player* player, int item_type, const rf::Vector3& pos, int respawn_ms);

// CTF flag / Salvage object transition. `player` may be null (timeout, reset);
// `team` is the flag's own team, or team_none for Salvage's neutral object.
void on_flag_event(FlagEventKind kind, uint8_t team, rf::Player* player, const rf::Vector3& pos);

// KOTH capture-point transition. `players` are the keys credited on an owner
// change and empty otherwise.
void on_point_event(int hill_uid, PointEventKind kind, uint8_t owner,
                    const std::vector<rf::Player*>& credited, bool locked);

// Bagman bag transition. `player` is the carrier for a pickup and the previous
// carrier for a drop; null for available and return.
void on_bagman_event(BagmanEventKind kind, rf::Player* player, const rf::Vector3& pos);
void on_bagman_pickup(rf::Player* player, const rf::Vector3& pos, BagmanFrom from);

// GunGame progression.
void on_gg_levelup(rf::Player* player, int level, int weapon_type);

// Payload cart transition. `players` are the attackers in the push zone. `push_time_ms` < 0 is
// reported as JSON null.
void on_payload_event(PayloadEventKind kind, float distance, const std::vector<rf::Player*>& players,
                      int push_time_ms = -1);
// `dt` seconds of the game_end summary's Payload push time (attackers pushing the cart) or block
// time (defenders blocking it).
void on_payload_zone_time(const std::vector<rf::Player*>& players, bool blocking, float dt);

// The Vampire mutator's life steal gave `player` this much effective health (armor counts double).
void on_lifesteal(rf::Player* player, float healed);

// The Scavenger target zone was placed.
void on_scav_zone(ScavZoneCause cause, const rf::Vector3& pos, float radius, int timeout_ms);

// Multiplayer vehicle lifecycle. `vehicle_id` is the hull's per-game id and `vehicle_class` its
// VehicleDamageClass. `team` is the hull's affiliation, -1 none, reported as team_none outside team
// game types. `player` may be null, `weapon_type` < 0 is null, and `cause` is sent only for destroyed
// and returned.
void on_vehicle_event(VehicleEventKind kind, uint32_t vehicle_id, int vehicle_class, int team,
                      const rf::Player* player, int weapon_type, VehicleEndCause cause, const rf::Vector3& pos);
// A player's seat in a vehicle changed; -1 is on foot.
void on_vehicle_seat(const rf::Player* player, uint32_t vehicle_id, int vehicle_class, int from_seat, int to_seat,
                     VehicleSeatReason reason);

// Ready-up match system, including Pit duels.
void on_match_start(int team_size, const std::vector<rf::Player*>& participants);
// `winner_team` is the team registry (team_none when not applicable) and
// `winner_player` is set only for duel-style matches.
void on_match_end(MatchResult result, uint8_t winner_team, rf::Player* winner_player);
// Same, for the ready-up match system, which stores no winner: the winning team or
// top-scoring player is derived from the live scores at the call site.
void on_match_end_derived(MatchResult result);

// Round system: a traditional round driven by rounds.cpp (a Pit duel, a Wipeout
// round). Nested inside a game (game_*). Numbering is 1-based and restarts each game.
void on_round_start(const std::vector<rf::Player*>& participants);
// Record who was still alive when a team round was decided, for the next on_round_end. Without a
// call the round reports no survivors list.
void note_round_survivors(const std::vector<rf::Player*>& survivors);
// `winner_team` is the team registry (team_none when not applicable) and
// `winner_player` is set only for duel-style rounds.
void on_round_end(RoundResult result, uint8_t winner_team, rf::Player* winner_player);

// Vote lifecycle. `vote_type` and `result` are the wire-frozen AfVoteType /
// AfVoteResult values and go out verbatim. `vote_type` is repeated on `vote_ended`
// so an outcome is typed without pairing to its call.
void on_vote_called(uint8_t vote_type, rf::Player* initiator, rf::Player* target,
                    const char* detail);
void on_vote_ended(uint8_t vote_type, uint8_t result, int yes, int no, int eligible);

// World / destruction events. Both are server-side only.

// A terrain carve happened -- classic geomod or the Alpine 1.3 brush-based kind,
// selected by `rf2_style`. `scale` is the carve radius in world units. `shooter` and
// `weapon_type` are best-effort attribution: a null `shooter` (chain reaction, level
// logic, environmental) and a `weapon_type` < 0 are reported as JSON null, and the
// weapon is nulled whenever the shooter is.
void on_geomod(const rf::Vector3& pos, float scale, bool rf2_style, rf::Player* shooter,
               int weapon_type);

// A destructible clutter prop was killed. `killer_handle` is the lethal blow's
// responsible entity, resolved to the attacking player (null = world/environmental);
// `weapon_type` < 0 and a null attacker are reported as JSON null. `damage_type` is the
// registry value. clutter_type (the clutter.tbl info_index) and the level uid
// are read off the object and reported as null when it carries none (info_index -1 for
// an alpine mesh, a negative uid for a runtime-spawned object).
void on_clutter_destroyed(rf::Clutter* clutter, int killer_handle, int weapon_type,
                          int damage_type);

// A breakable detail brush (level geometry -- stock RF's destructible glass, extended by
// Alpine to several materials) shattered. `material` is the rf::DetailMaterial registry
// value and is the point of the event. `room_uid` is the detail room's uid,
// reported as null when negative. `killer_handle` is the lethal blow's responsible entity,
// resolved to the attacking player (null = world/environmental); `weapon_type` < 0 and a
// null attacker are reported as JSON null. `damage_type` is the registry value.
void on_detail_brush_destroyed(uint8_t material, int room_uid, int killer_handle,
                               int weapon_type, int damage_type, const rf::Vector3& pos);

// The text `sv_afstats_events_status` prints, as one newline-joined string with no
// trailing newline, so the rcon handler can return it to the caller.
std::string build_afstats_events_status_output();

// Register console commands. Called from fflink::do_patch().
void do_patch();

// Sender pulse. Called from fflink::do_frame().
void do_frame();

// Clean shutdown: close any open game and make one bounded attempt to flush.
// Never blocks for more than about two seconds, and there is no delivery
// guarantee.
void on_shutdown();

} // namespace afstats
