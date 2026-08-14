#pragma once

#include <cstdint>
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

// Wire values from the round_end `end_type` registry.
enum class RoundEndType : uint8_t
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

// Wire values from the match_end `result` registry.
enum class MatchResult : uint8_t
{
    completed = 0,
    canceled = 1,
};

// Wire values from the round_start `match_state` registry. Stamped on every
// round_start so warmup play is distinguishable from match play at round
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
// alive; emits `player_leave` with the partial-round summary.
void on_player_leave(rf::Player* player);

// The server processed a name change for this player. Emits `player_rename` with
// the finalized name; call after the stock handler has applied it. `prev_name` is the
// name the player held before this change (what the stream last reported): an unchanged
// name emits nothing, and successive changes are rate-floored.
void on_player_rename(rf::Player* player, const char* name, std::string_view prev_name);

// A level finished loading server-side. Emits `round_start`.
void on_round_start();

// Record why the current round is ending. First writer wins; consumed and reset
// by on_round_end(). Without a call the round is reported as a manual change.
void note_round_end_type(RoundEndType end_type);

// The server entered limbo, i.e. the round is over. Emits `round_end`.
void on_round_end();

// A lethal blow landed. Arguments mirror what the af_kill_info path already
// computed: `weapon_type` < 0 and a null `killer` are reported as JSON null,
// `kill_flags` is the af_kill_info_flags bitfield and goes on the wire verbatim,
// and `assist_player_ids` is the attribution list in most-recent-first order.
void on_kill(rf::Player* victim, rf::Player* killer, int weapon_type, int damage_type,
             uint8_t kill_flags, const std::vector<uint8_t>& assist_player_ids,
             const rf::Vector3& victim_pos, const rf::Vector3* killer_pos);

// A player spawned server-side.
void on_spawn(rf::Player* player, const rf::Vector3& pos);

// Damage was applied to a player. Feeds the windowed `damage` aggregate and, when
// `direct_hit` is set, the `hit`/`hit_head` side of the `accuracy` aggregate --
// splash contributes damage but is never a projectile hit. A null
// `attacker` is environmental. `amount` is effective post-armor damage.
void on_damage(rf::Player* attacker, rf::Player* victim, int weapon_type, int damage_type,
               float amount, bool direct_hit, bool headshot);

// A player pulled a trigger. `projectiles` is the shot's projectile count from the
// weapon table, so pellet weapons account exactly.
void on_weapon_fired(rf::Player* player, int weapon_type, uint32_t projectiles);

// Out-of-band player state change. `value` is only read for
// StatusKind::handicap.
void on_status(rf::Player* player, StatusKind kind, int value = 0);

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

// Ready-up match system, including Pit duels.
void on_match_start(int team_size, const std::vector<rf::Player*>& participants);
// `winner_team` is the team registry (team_none when not applicable) and
// `winner_player` is set only for duel-style matches.
void on_match_end(MatchResult result, uint8_t winner_team, rf::Player* winner_player);
// Same, for the ready-up match system, which stores no winner: the winning team or
// top-scoring player is derived from the live scores at the call site.
void on_match_end_derived(MatchResult result);

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

// Register console commands. Called from fflink::do_patch().
void do_patch();

// Sender pulse. Called from fflink::do_frame().
void do_frame();

// Clean shutdown: close any open round and make one bounded attempt to flush.
// Never blocks for more than about two seconds, and there is no delivery
// guarantee.
void on_shutdown();

} // namespace afstats
