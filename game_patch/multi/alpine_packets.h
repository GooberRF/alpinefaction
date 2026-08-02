#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <common/rfproto.h>
#include "gametype.h"
#include "../multi/server_internal.h"
#include "../rf/multi.h"

#pragma pack(push, 1)

// Forward declarations
namespace rf
{
    struct Object;
    struct Player;
    struct Vector3;
    struct Matrix3;
    struct Entity;
}

enum class af_packet_type : uint8_t
{
    af_ping_location_req = 0x50,        // Alpine 1.1
    af_ping_location = 0x51,            // Alpine 1.1
    af_damage_notify = 0x52,            // Alpine 1.1
    af_obj_update = 0x53,               // Alpine 1.1
    af_client_req = 0x55,               // Alpine 1.2
    af_just_spawned_info = 0x56,        // Alpine 1.2
    af_koth_hill_state = 0x57,          // Alpine 1.2
    af_koth_hill_captured = 0x58,       // Alpine 1.2
    af_just_died_info = 0x59,           // Alpine 1.2
    af_server_info = 0x5A,              // Alpine 1.2
    af_spectate_start = 0x5B,           // Alpine 1.2
    af_spectate_notify = 0x5C,          // Alpine 1.2
    af_server_msg = 0x5D,               // Alpine 1.2
    af_server_req = 0x5E,               // Alpine 1.2.1
    af_server_bot_control = 0x5F,       // Alpine 1.3
    af_bagman_state = 0x60,             // Alpine 1.4
    af_pit_roster = 0x61,               // Alpine 1.4
    af_gungame_order = 0x62,            // Alpine 1.4
};

struct af_ping_location_req_packet
{
    RF_GamePacketHeader header;
    RF_Vector pos;
};

struct af_ping_location_packet
{
    RF_GamePacketHeader header;
    uint8_t player_id;
    RF_Vector pos;
};

struct af_damage_notify_packet
{
    RF_GamePacketHeader header;
    uint8_t player_id;
    uint16_t damage;
    uint8_t flags;
};

struct af_obj_update // members of af_obj_update_packet
{
    uint32_t obj_handle;
    uint8_t current_primary_weapon;
    uint8_t ammo_type;
    uint16_t clip_ammo;
    uint16_t reserve_ammo;
};

struct af_obj_update_packet
{
    RF_GamePacketHeader header;
    af_obj_update objects[];
};

enum class af_client_req_type : uint8_t
{
    af_req_handicap = 0x0,
    af_req_server_cfg = 0x1,
    af_req_spray = 0x2,
    af_req_character = 0x3,
    af_req_ready = 0x4,        // Alpine 1.4 (1 byte: action 0=unready,1=ready,2=toggle)
    af_req_pit_queue = 0x5,    // Alpine 1.4 (1 byte: action 0=leave,1=join,2=toggle)
    af_req_vote_call = 0x6,    // Alpine 1.4 (variable, see af_build_vote_call_payload)
    af_req_vote_cast = 0x7,    // Alpine 1.4 (1 byte: 0 = no, 1 = yes)
    af_req_vote_cancel = 0x8,  // Alpine 1.4 (no additional data)
    af_req_vote_options = 0x9, // Alpine 1.4 (5 bytes: flags + known_generation)
    af_req_jetpack_state = 0xA, // Alpine 1.4 (1 byte: on 0/1)
};

// Frozen wire constants, values can NEVER be reordered or changed.
enum class AfVoteType : uint8_t
{
    Kick = 0,
    Level = 1,
    Match = 2,
    Extend = 3,
    Restart = 4,
    Next = 5,
    Random = 6,
    Previous = 7,
    CancelMatch = 8,
};
// Number of vote types this build knows about.
// NOT a wire limit: a newer server may run a type with a higher value,
// and a client tolerates that. Only the server uses it as a rejection bound,
// on incoming vote calls.
constexpr uint8_t af_vote_type_count = 9;
static_assert(af_vote_type_count <= 32, "enabled_vote_mask is a u32");

// "no gametype override" sentinel in vote-call payloads.
constexpr uint8_t af_vote_gametype_none = 0xFF;

// Extend vote duration, in minutes. Shared by the panel's numeric entry, the
// chat menu's quick entry and the server's validation of the incoming payload,
// so all three agree on what a legal request looks like.
constexpr uint8_t af_vote_extend_min_minutes = 1;
constexpr uint8_t af_vote_extend_max_minutes = 60;
constexpr uint8_t af_vote_extend_default_minutes = 5;

// af_sreq_vote_state `event` byte. FROZEN wire constants.
enum class AfVoteStateEvent : uint8_t
{
    Start = 0,
    Update = 1,
    End = 2,
};

// af_sreq_vote_state end-event `result` byte. FROZEN wire constants.
enum class AfVoteResult : uint8_t
{
    Passed = 0,
    Failed = 1,
    Canceled = 2,
    TimedOut = 3,
};

enum af_vote_state_flags : uint8_t
{
    // the recipient started this vote
    AF_VOTE_STATE_FLAG_OWNER = 1 << 0,
    // the recipient joined while the vote was in progress
    AF_VOTE_STATE_FLAG_SYNC = 1 << 1,
};

enum af_vote_end_flags : uint8_t
{
    // The vote PASSED and its outcome is being applied.
    // Independent of result, which is how the vote ended.
    AF_VOTE_END_FLAG_PASSED = 1 << 0,
};

// Version of the reassembled vote-options blob. A client accepts any blob
// whose version is <= the version it was built for and rejects anything greater.
//
// Repeated records in the blob are length-prefixed, so additions to the blob
// are not compatibility breaking. This version should be incremented only if
// the core format changes - like redefining a field or reordering/removing them.
constexpr uint8_t af_vote_options_blob_version = 1;

// af_sreq_vote_options_data stream framing. The blob is pushed as
// Begin -> Data* -> End over the ordered reliable channel, so no chunk index or
// chunk count is needed and there is no size ceiling.
enum class AfVoteOptionsStream : uint8_t
{
    Begin = 0, // u32 generation, u32 total_bytes
    Data = 1,  // u32 generation, bytes[]  (length comes from the packet header)
    End = 2,   // u32 generation
};

// Hard ceiling on a reassembled vote-options blob.
// Defense in depth, should never be legitimately reached.
constexpr uint32_t af_vote_options_max_blob_size = 1024 * 1024;

enum af_vote_options_req_flags : uint8_t
{
    // The request carries the generation of a blob the client already parsed, so
    // the server can skip re-sending an identical one.
    AF_VOTE_OPTIONS_REQ_HAS_CACHE = 1 << 0,
};

enum af_vote_gametype_flags : uint8_t
{
    AF_VOTE_GAMETYPE_FLAG_TEAM = 1 << 0,
};

// Per-level flags in the vote-options blob's level section.
enum af_vote_level_flags : uint8_t
{
    // The server's vote_level allow-list accepts this level. When clear, a vote
    // naming this level is rejected outright whatever game type is selected —
    // the blob still lists it (it is in the rotation) but it is not votable.
    AF_VOTE_LEVEL_FLAG_ALLOWED = 1 << 0,
};

// The `baseline_kind` byte appended after a level entry's flags: which mutator
// set the vote panel pre-selects when that level is picked.
enum class AfVoteLevelBaseline : uint8_t
{
    // Use the blob's trailing base mutator set.
    InheritBase = 0,
    // A declaration set follows.
    // An empty one is meaningful: it says this level runs no mutators
    // at all, which is NOT the same as inheriting.
    Explicit = 1,
};

// Server-wide vote flags, the `server_flags` byte of the vote-options blob.
enum af_vote_server_flags : uint8_t
{
    // vote_level.only_allow_gametype_prefix is on, so each level's
    // valid_gametype_mask actually restricts something and the UI may say so.
    AF_VOTE_SERVER_FLAG_GAMETYPE_PREFIX = 1 << 0,
};

struct HandicapPayload
{
    uint8_t amount = 0;
};

struct SprayReqPayload
{
    uint16_t texture_id = 0;
    RF_Vector pos = {};
    RF_Vector normal = {};
};
static_assert(sizeof(SprayReqPayload) == 26);

struct CharacterPayload
{
    uint8_t character_index = 0;
};

struct ReadyReqPayload
{
    uint8_t action = 0; // 0 = unready, 1 = ready, 2 = toggle
};

struct PitQueueReqPayload
{
    uint8_t action = 0; // 0 = leave, 1 = join, 2 = toggle
};

struct VoteCastReqPayload
{
    uint8_t is_yes = 0; // 0 = no, 1 = yes
};

struct VoteOptionsReqPayload
{
    uint8_t flags = 0;              // af_vote_options_req_flags
    uint32_t known_generation = 0;  // meaningful only with AF_VOTE_OPTIONS_REQ_HAS_CACHE
};

// Jetpacks mutator: the client owns its own movement and just tells the server
// when its thrusters turn on or off so the effects can be relayed to everyone.
struct JetpackStateReqPayload
{
    uint8_t on = 0;
};

using af_client_payload = std::variant<HandicapPayload, SprayReqPayload, CharacterPayload,
                                       ReadyReqPayload, PitQueueReqPayload, VoteCastReqPayload,
                                       VoteOptionsReqPayload, JetpackStateReqPayload, std::monostate>;

struct af_client_req_packet
{
    RF_GamePacketHeader header;
    af_client_req_type req_type;
    af_client_payload payload;
};

enum class af_server_req_type : uint8_t
{
    af_sreq_should_gib = 0x0,
    af_sreq_teleport_entity = 0x1,     // Alpine 1.4
    af_sreq_spray = 0x2,               // Alpine 1.4
    af_sreq_ready_prompt = 0x3,        // Alpine 1.4 (1 byte: state 0/1/2)
    af_sreq_pit_queue_state = 0x4,     // Alpine 1.4 (3 bytes: flags, position, total)
    af_sreq_vote_state = 0x5,          // Alpine 1.4 (variable, see AfVoteStateEvent)
    af_sreq_vote_options_data = 0x6,   // Alpine 1.4 (chunked vote-options blob)
    af_sreq_kill_info = 0x7,           // Alpine 1.4 (5 bytes: victim, killer, weapon, flags, damage_type)
    af_sreq_entity_on_fire = 0x8,      // Alpine 1.4 (5 bytes: obj_handle, on)
    af_sreq_jetpack_state = 0x9,       // Alpine 1.4 (5 bytes: obj_handle, on)
};

struct ShouldGibPayload
{
    uint32_t obj_handle = 0;
};

struct TeleportEntityPayload
{
    uint32_t obj_handle = 0;
    RF_Vector pos = {};
    RF_Matrix orient = {};
    RF_Vector vel = {};
};

enum af_spray_flags : uint8_t
{
    AF_SPRAY_FLAG_SILENT = 1 << 0,
};

struct SprayPayload
{
    uint8_t player_id = 0;
    uint16_t texture_id = 0;
    RF_Vector pos = {};
    RF_Vector normal = {};
    uint8_t flags = 0; // af_spray_flags
};
static_assert(sizeof(SprayPayload) == 28);

struct ReadyPromptPayload
{
    // Tri-state (1 byte on the wire):
    //   0 = pre-match NOT active (clear flag + hide prompt)
    //   1 = pre-match active, show the ready-up prompt
    //   2 = pre-match active, you are ready (hide prompt, keep flag set)
    uint8_t state = 0;
};

struct PitQueueStatePayload
{
    uint8_t flags = 0;    // bit0 = queued, bit1 = is_dueler, bit2 = should spectate
    uint8_t position = 0; // 1-based position among waiting queue, 0 if n/a
    uint8_t total = 0;    // waiting-queue size
};

enum af_kill_info_flags : uint8_t
{
    // Non-headshot, non-legshot, non-splash = torso shot
    AF_KILL_FLAG_HEADSHOT = 1 << 0, // meaningful only for direct hits
    AF_KILL_FLAG_SPLASH   = 1 << 1, // lethal damage came from splash
    AF_KILL_FLAG_MELEE    = 1 << 2,
    AF_KILL_FLAG_SUICIDE  = 1 << 3,
    AF_KILL_FLAG_LEGSHOT  = 1 << 4, // meaningful only for direct hits
    AF_KILL_FLAG_GIBBED   = 1 << 5,
};

// Decorates the stock obj_kill packet, which carries no weapon.
struct KillInfoPayload
{
    uint8_t killed_player_id = 0xFF;
    uint8_t killer_player_id = 0xFF; // 0xFF = no killer player (world death)
    uint8_t weapon_type = 0xFF;
    uint8_t flags = 0;  // af_kill_info_flags
    uint8_t damage_type = 0xFF;
};
static_assert(sizeof(KillInfoPayload) == 5);

constexpr uint8_t af_kill_info_max_assists = 8;

// Used by the Flaming Enemies mutator.
// This is purely visual on the client. All fire damage is dealt by the server.
struct EntityOnFirePayload
{
    uint32_t obj_handle = 0;
    uint8_t on = 0; // 1 = ignite, 0 = extinguish
};
static_assert(sizeof(EntityOnFirePayload) == 5);

// Used by the Jetpacks mutator. Purely visual: the thrusting client already
// applied its own physics, this only drives the effects on every other client.
struct EntityJetpackPayload
{
    uint32_t obj_handle = 0;
    uint8_t on = 0; // 1 = thrusting, 0 = idle
};
static_assert(sizeof(EntityJetpackPayload) == 5);

using af_server_req_payload = std::variant<ShouldGibPayload, TeleportEntityPayload, SprayPayload,
                                           ReadyPromptPayload, PitQueueStatePayload, EntityOnFirePayload,
                                           EntityJetpackPayload>;

struct af_server_req_packet
{
    RF_GamePacketHeader header;
    af_server_req_type req_type;
    af_server_req_payload payload;
};

enum class af_just_spawned_info_type : uint8_t
{
    af_loadout = 0x00,
};

struct LoadoutEntry
{
    uint8_t weapon_index;
    uint32_t ammo;
};

struct af_just_spawned_info_packet
{
    RF_GamePacketHeader header;
    uint8_t info_type;  // af_just_spawned_info_type
    uint8_t data[];     // type-specific payload
};

enum af_player_info_flags : uint8_t
{
    AF_PLAYER_FLAG_BOT       = 1 << 0,
    AF_PLAYER_FLAG_BROWSER   = 1 << 1,
    AF_PLAYER_FLAG_SPECTATOR = 1 << 2,
    AF_PLAYER_FLAG_IDLE      = 1 << 3,
    AF_PLAYER_FLAG_TEAM_BLUE = 1 << 4,
};

// Per-segment framing. The payload bytes of each segment are concatenated in
// `sequence` order (grouped by `response_id`) to form one logical payload
// described by `af_player_info_payload_header` below.
struct af_player_info_packet
{
    RF_GamePacketHeader hdr; // type = pf_packet_type::players (0xA1)
    uint8_t version;        // 2
    uint8_t response_id;
    uint8_t sequence;
    uint8_t total_segments;
    // segmented payload follows
};

// Reassembled payload layout:
//   af_player_info_payload_header preamble
//   char level_filename[] (null-terminated)
//   player entries (variable length per entry):
//     uint8_t  flags
//     int16_t  score
//     uint16_t kills
//     uint16_t deaths
//     uint16_t caps
//     char     name[] (null-terminated)
struct af_player_info_payload_header
{
    uint16_t red_score;     // 0 in non-team game types
    uint16_t blue_score;    // 0 in non-team game types
    uint32_t time_left_seconds; // UINT32_MAX if no time limit, 0 if time expired
    uint32_t af_flags;      // af_server_info_flags bitfield
    uint8_t  game_type;     // rf::NetGameType
};

constexpr uint8_t af_player_info_packet_version = 2;

struct af_koth_hill_state_packet
{
    RF_GamePacketHeader header;
    uint8_t hill_uid;
    uint8_t ownership;
    uint8_t steal_dir;
    uint8_t state;
    uint8_t lock_status;
    uint8_t capture_progress;
    uint8_t num_red_players;
    uint8_t num_blue_players;
    uint16_t red_score;
    uint16_t blue_score;
};

struct af_bagman_state_packet
{
    RF_GamePacketHeader header;
    uint8_t carrier_player_id;
    uint8_t state;
    uint16_t return_time_left_ms;
    uint16_t red_team_score;
    uint16_t blue_team_score;
    int16_t  carrier_score;
};

struct af_koth_hill_captured_packet
{
    RF_GamePacketHeader header;
    uint8_t hill_uid;
    uint8_t ownership;
    uint8_t num_new_owner_players;
    //uint8_t new_owner_player_ids[]; // appended on the wire
};

// Pit roster: replicates each non-browser player's Pit role to all clients so
// the scoreboard can group them (queued-vs-not-queued is otherwise server-only).
// role: 0 = dueler, 1 = queued, 2 = not queued.
// order: 1-based queue position for queued players (front = 1); 0 otherwise.
struct af_pit_roster_entry
{
    uint8_t player_id;
    uint8_t role;
    uint8_t order;
};
static_assert(sizeof(af_pit_roster_entry) == 3);

struct af_pit_roster_packet
{
    RF_GamePacketHeader header;
    uint8_t count;
    //af_pit_roster_entry entries[]; // appended on the wire
};

// Gun Game per-player weapon order sent by server to client;
// client uses for local HUD display.
struct af_gungame_order_entry
{
    uint16_t threshold;
    uint8_t weapon_index;
};
static_assert(sizeof(af_gungame_order_entry) == 3);

struct af_gungame_order_packet
{
    RF_GamePacketHeader header;
    uint8_t count;
    //af_gungame_order_entry entries[]; // appended on the wire
};

enum af_just_died_info_flags
{
    JDI_RESPAWN_ALLOWED = 0x1,
    JDI_FORCE_RESPAWN = 0x2
};

struct af_just_died_info_packet
{
    RF_GamePacketHeader header;
    uint8_t flags;
    uint16_t spawn_delay;
};

enum af_server_info_flags : uint32_t {
    SIF_NONE = 0,
    SIF_POSITION_SAVING = 1u << 0,
    SIF_UNUSED = 1u << 1,
    SIF_ALLOW_FULLBRIGHT_MESHES = 1u << 2,
    SIF_ALLOW_LIGHTMAPS_ONLY = 1u << 3,
    SIF_ALLOW_NO_SCREENSHAKE = 1u << 4,
    SIF_NO_PLAYER_COLLIDE = 1u << 5,
    SIF_ALLOW_NO_MUZZLE_FLASH_LIGHT = 1u << 6,
    SIF_CLICK_LIMITER = 1u << 7,
    SIF_ALLOW_UNLIMITED_FPS = 1u << 8,
    SIF_GAUSSIAN_SPREAD = 1u << 9,
    SIF_LOCATION_PINGING = 1u << 10,
    SIF_DELAYED_SPAWNS = 1u << 11,
    SIF_SERVER_CFG_CHANGED = 1u << 12,
    SIF_GEO_CHUNK_PHYSICS = 1u << 13,
    SIF_ALLOW_FOOTSTEPS = 1u << 14,
    SIF_ALLOW_OUTLINES = 1u << 15,
    SIF_ALLOW_OUTLINES_XRAY = 1u << 16,
    SIF_CLEAR_STALE_MOVEMENT_INPUT = 1u << 17,
    SIF_MANUAL_LEVEL_LOAD = 1u << 18,
    SIF_ALLOW_SPRAYS = 1u << 19,
    SIF_FEATURED_NO_CLIP = 1u << 20,
    SIF_RELOAD_ON_KILL = 1u << 21,
    SIF_SUPER_DRAIN = 1u << 22,
    SIF_JETPACKS = 1u << 23,
};

// Subset of `rf::NetGameFlags`.
enum rf_server_info_flags : uint8_t {
    RFSIF_NONE = 0,
    RFSIF_WEAPON_STAY = 1u << 0,
    RFSIF_FORCE_RESPAWN = 1u << 1,
    RFSIF_TEAM_DAMAGE = 1u << 2,
    RFSIF_FALL_DAMAGE = 1u << 3,
    RFSIF_BALANCE_TEAMS = 1u << 4,
};

struct af_server_info_packet
{
    RF_GamePacketHeader header;
    uint8_t rf_flags = 0;  // subset of rf::NetGameFlags
    uint8_t game_type = 0; // rf::NetGameType
    uint32_t af_flags = 0;
    uint32_t win_condition = 0; // gametype-dependent
    uint16_t semi_auto_cooldown = 0;
};

struct af_spectate_start_packet {
    RF_GamePacketHeader header;
    uint8_t spectatee_id;
};

struct af_spectate_notify_packet {
    RF_GamePacketHeader header;
    uint8_t spectator_id;
    bool does_spectate;
};

enum af_server_msg_type : uint8_t {
    AF_SERVER_MSG_TYPE_REMOTE_SERVER_CFG = 0x1,
    AF_SERVER_MSG_TYPE_AUTOMATED_CHAT = 0x2,
    AF_SERVER_MSG_TYPE_REMOTE_SERVER_CFG_EOF = 0x3,
    AF_SERVER_MSG_TYPE_CONSOLE = 0x4,
    AF_SERVER_MSG_TYPE_HUD_NOTIFICATION = 0x5, // Alpine 1.4 ( text follows after the fixed prefix)
    AF_SERVER_MSG_TYPE_ROUND_COUNTDOWN = 0x6,  // Alpine 1.4
    AF_SERVER_MSG_TYPE_PLAY_CUSTOM_SOUND = 0x7, // Alpine 1.4
};

#pragma pack(push, 1)
struct af_round_countdown_payload {
    uint8_t duration_seconds;
};
#pragma pack(pop)
static_assert(sizeof(af_round_countdown_payload) == 1);

// Must reference a custom sound ID.
#pragma pack(push, 1)
struct af_play_custom_sound_payload {
    uint16_t custom_sound_id;
};
#pragma pack(pop)
static_assert(sizeof(af_play_custom_sound_payload) == 2);

// Text follows immediately after this struct.
// Keep in sync with HudNotificationType in hud.h.
#pragma pack(push, 1)
struct af_hud_notification_prefix {
    int8_t duration_seconds;    // negative = persistent
    uint8_t notification_type;  // cast to HudNotificationType on the client
    uint8_t fade_on_expire;     // bool
};
#pragma pack(pop)
static_assert(sizeof(af_hud_notification_prefix) == 3);

struct af_server_msg_packet {
    RF_GamePacketHeader header;
    uint8_t type;
    char data[];
};

enum class af_bot_control_type : uint8_t
{
    go_inactive        = 0x00,
    go_active          = 0x01,
    disconnect_bot     = 0x02,
    update_personality = 0x03,
    update_skill       = 0x04,
    update_identity    = 0x05,
};

enum class af_personality_field : uint8_t
{
    attack_style = 0x00,
    preferred_engagement_near = 0x01,
    preferred_engagement_far = 0x02,
    super_pickup_bias = 0x03,
    revisit_avoidance_bias = 0x04,
    retrace_avoidance_bias = 0x05,
    retrace_lookback_waypoints = 0x06,
    decision_aggression_bias = 0x07,
    decision_efficiency_bias = 0x08,
    decision_risk_tolerance = 0x09,
    path_smoothing_bias = 0x0A,
    corner_centering_bias = 0x0B,
    roam_intensity_bias = 0x0C,
    navigation_strafe_bias = 0x0D,
    crouch_route_avoidance_bias = 0x0E,
    stuck_goal_retry_limit = 0x0F,
    goal_commitment_bias = 0x10,
    eliminate_target_commitment_bias = 0x11,
    opportunism_bias = 0x12,
    retreat_health_threshold = 0x13,
    retreat_armor_threshold = 0x14,
    replenish_health_threshold = 0x15,
    replenish_armor_threshold = 0x16,
    seek_weapon_bias = 0x17,
    satisfactory_weapon_threshold = 0x18,
    preferred_weapon_ammo_fill_threshold = 0x19,
    replenish_bias = 0x1A,
    power_position_bias = 0x1B,
    weapon_switch_bias = 0x1C,
    min_weapon_switch_cooldown_ms = 0x1D,
    crouch_combat_bias = 0x1E,
    jump_combat_bias = 0x1F,
    dodge_combat_bias = 0x20,
    raw_aggression_bias = 0x21,
    camping_bias = 0x22,
    easy_frag_bias = 0x23,
    retaliation_bias = 0x24,
    combat_readiness_threshold = 0x25,
    deathmatch_kill_focus_bias = 0x26,
    ctf_capture_priority_bias = 0x27,
    ctf_flag_recovery_bias = 0x28,
    ctf_hold_enemy_flag_safety_bias = 0x29,
    ctf_hold_carrier_hunt_bias = 0x2A,
    quirk_mask_low = 0x2B,
    quirk_mask_high = 0x2C,
    pickup_switch_chance_without_preferences = 0x2D,
    taunt_on_kill_chance = 0x2E,
    gg_on_map_end_chance = 0x2F,
    hello_on_join_chance = 0x30,
    red_faction_response_chance = 0x31,
};

enum class af_skill_field : uint8_t
{
    base_skill = 0x00,
    aim_profile_scale = 0x01,
    decision_profile_scale = 0x02,
    survivability_maintenance_bias = 0x03,
    alertness = 0x04,
    fov_degrees = 0x05,
    target_focus_bias = 0x06,
    weapon_switch_likelihood = 0x07,
    crouch_likelihood = 0x08,
    jump_likelihood = 0x09,
    dodge_likelihood = 0x0A,
};

#pragma pack(pop)

inline constexpr uint8_t kBotControlPacketVersion = 1;
inline constexpr uint8_t kMaxPresetNameLen = 31;

// Decoded af_req_vote_call payload. Not a wire struct — it holds owning types,
// so it must stay outside the packed region above.
struct AfVoteCallParams
{
    AfVoteType type = AfVoteType::Kick;
    uint8_t target_player_id = 0xFF;      // Kick
    uint8_t team_size = 0;                // Match
    std::string level;                    // Level (required) / Match (empty = current)
    uint8_t gametype = af_vote_gametype_none;
    uint8_t extend_minutes = af_vote_extend_default_minutes;
    std::vector<VoteMutatorInput> mutators;
};

bool af_process_packet(const void* data, int len, const rf::NetAddr& addr, rf::Player* player);
void af_send_packet(rf::Player* player, const void* data, int len, bool is_reliable);

void af_send_ping_location_req_packet(rf::Vector3* pos);
static void af_process_ping_location_req_packet(const void* data, size_t len, const rf::NetAddr& addr);
void af_send_ping_location_packet_to_team(rf::Vector3* pos, uint8_t player_id, rf::ubyte team);
void af_send_ping_location_packet_to_all(rf::Vector3* pos, uint8_t player_id);
static void af_process_ping_location_packet(const void* data, size_t len, const rf::NetAddr& addr);
void af_send_damage_notify_packet(uint8_t player_id, float damage, bool died, rf::Player* player);
static void af_process_damage_notify_packet(const void* data, size_t len, const rf::NetAddr& addr);
void af_send_obj_update_packet(rf::Player* player);
static void af_process_obj_update_packet(const void* data, size_t len, const rf::NetAddr& addr);
void af_send_client_req_packet(const af_client_req_packet& packet, bool is_reliable = false);
static void af_process_client_req_packet(const void* data, size_t len, const rf::NetAddr& addr);
void af_send_character_request(int character_index);
void af_send_server_req_packet(const af_server_req_packet& packet, rf::Player* player);
void af_send_should_gib_req(uint32_t obj_handle);
void af_send_kill_info(rf::Player* killed_player);
void af_send_entity_on_fire(uint32_t obj_handle, bool on);
void af_send_jetpack_state_request(bool on);
void af_send_jetpack_state(uint32_t obj_handle, bool on);
void af_send_teleport_entity_req(uint32_t obj_handle, const rf::Vector3& pos, const rf::Matrix3& orient, const rf::Vector3& vel);
void af_send_spray_to_player(uint8_t player_id, uint16_t texture_id, const rf::Vector3& pos, const rf::Vector3& normal, uint8_t flags, rf::Player* player);
void af_broadcast_spray(uint8_t player_id, uint16_t texture_id, const rf::Vector3& pos, const rf::Vector3& normal);
static void af_process_server_req_packet(const void* data, size_t len, const rf::NetAddr& addr);
void af_send_just_spawned_loadout(rf::Player* to_player, std::vector<WeaponLoadoutEntry> loadout);
static void af_process_just_spawned_info_packet(const void* data, size_t len, const rf::NetAddr& addr);
void af_send_koth_hill_state_packet(rf::Player* player, const HillInfo& h, const Presence& pres); // sent to new joiners
void af_send_koth_hill_state_packet_to_all(const HillInfo& h, const Presence& pres);
static void af_process_koth_hill_state_packet(const void* data, size_t len, const rf::NetAddr&);
void af_send_bagman_state_packet(rf::Player* player);
void af_send_bagman_state_packet_to_all();
void af_process_bagman_state_packet(const void* data, size_t len, const rf::NetAddr&);
void af_send_pit_roster(rf::Player* player, const std::vector<af_pit_roster_entry>& roster);
void af_broadcast_pit_roster(const std::vector<af_pit_roster_entry>& roster);
void af_process_pit_roster_packet(const void* data, size_t len, const rf::NetAddr&);
void af_send_gungame_order(rf::Player* player, const std::vector<af_gungame_order_entry>& order);
void af_process_gungame_order_packet(const void* data, size_t len, const rf::NetAddr&);
void af_send_koth_hill_captured_packet_to_all(uint8_t hill_uid, HillOwner owner, const std::vector<uint8_t>& new_owner_player_ids);
static void af_process_koth_hill_captured_packet(const void* data, size_t len, const rf::NetAddr&);
void af_send_just_died_info_packet(rf::Player* to_player, bool respawn_allowed, bool force_respawn, uint16_t spawn_delay);
static void af_process_just_died_info_packet(const void* data, size_t len, const rf::NetAddr& addr);
void af_send_server_info_packet(rf::Player* player);
void af_send_server_info_packet_to_all();
static void af_process_server_info_packet(const void* data, size_t len, const rf::NetAddr&);
void af_send_spectate_start_packet(const rf::Player* spectatee);
void af_process_spectate_start_packet(const void* data, size_t len, const rf::NetAddr&);
void af_send_spectate_notify_packet(rf::Player* player, const rf::Player* spectator, bool does_spectate);
void af_process_spectate_notify_packet(const void* data, size_t len, const rf::NetAddr&);
void af_send_server_cfg(rf::Player* player);
void af_process_server_msg_packet(const void* data, size_t len, const rf::NetAddr&);
void af_broadcast_automated_chat_msg(std::string_view msg);
void af_send_automated_chat_msg(std::string_view msg, rf::Player* player, bool tell_server = false);
void af_broadcast_vote_legacy_chat_msg(std::string_view msg);
void af_broadcast_hud_notification(
    std::string_view text, int duration_seconds, int notification_type, bool fade_on_expire = true);
void af_send_hud_notification(
    std::string_view text, int duration_seconds, int notification_type, bool fade_on_expire, rf::Player* player);

// Instruct clients to render the big number countdown for the next N seconds (max 10).
void af_broadcast_round_countdown(int duration_seconds);

void af_broadcast_play_custom_sound(int custom_sound_id);
void af_send_play_custom_sound(int custom_sound_id, rf::Player* player);
void af_send_server_console_msg(std::string_view msg, rf::Player* player, bool tell_server = false);

// client requests
void af_send_handicap_request(uint8_t amount);
void af_send_server_cfg_request();
void af_send_spray_request(uint16_t texture_id, const rf::Vector3& pos, const rf::Vector3& normal);
void af_send_ready_request(uint8_t action);      // 0 = unready, 1 = ready, 2 = toggle
void af_send_pit_queue_request(uint8_t action);  // 0 = leave, 1 = join, 2 = toggle

// vote system (client -> server)
void af_send_vote_call(const AfVoteCallParams& params);
void af_send_vote_cast(bool is_yes_vote);
void af_send_vote_cancel();
void af_send_vote_options_request(bool has_cache, uint32_t known_generation);

// vote system (server -> client)
// `initiator_name` may be empty when the vote owner is no longer connected.
// `is_sync` marks a mid-vote resync for a player who just joined.
void af_send_vote_state_start(rf::Player* player, AfVoteType type, uint16_t time_remaining_sec,
                              uint8_t yes, uint8_t no, uint8_t remaining, bool is_owner, bool is_sync,
                              std::string_view initiator_name, std::string_view title);
void af_send_vote_state_update(rf::Player* player, uint8_t yes, uint8_t no, uint8_t remaining);
// `passed` is independent of `result` (a timed-out vote can pass); `detail` is
// the outcome line legacy clients receive as chat, so both see the same wording.
void af_send_vote_state_end(rf::Player* player, AfVoteResult result, bool passed,
                            std::string_view detail);
void af_send_vote_options_data(rf::Player* player);

// server -> client state (Pit + match ready system)
void af_send_ready_prompt(rf::Player* player, uint8_t state); // 0/1/2 (see ReadyPromptPayload)
void af_send_pit_queue_state(rf::Player* player, uint8_t flags, uint8_t pos, uint8_t total);

// server bot control
void af_send_bot_control_simple(rf::Player* player, af_bot_control_type subtype);
void af_send_bot_control_update_personality(rf::Player* player, const ServerBotConfig& config);
void af_send_bot_control_update_skill(rf::Player* player, const ServerBotConfig& config);
void af_send_bot_control_update_identity(rf::Player* player, const std::string& name, int32_t character_index);
void af_send_bot_config(rf::Player* player, const ServerBotConfig& config,
                        const std::string& resolved_name, int32_t resolved_character);
void af_process_bot_control_packet(const void* data, size_t len, const rf::NetAddr& addr);

// player info response (sent to online server browsers)
void af_send_player_info_response(const rf::NetAddr& addr);
