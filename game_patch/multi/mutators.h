#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <toml++/toml.hpp>
#include "server_internal.h"

namespace rf
{
    struct Entity;
    struct Item;
    struct Object;
    struct Player;
    struct Vector3;
    struct Weapon;
}

// Mutators quickly reconfigure a set of gameplay rules to produce an alternate
// style of play or modify the experience in some way.
//
// Mutators are applied AFTER the gametype defaults but BEFORE any explicitly-set
// rule keys in the same scope, so manual keys always win. They apply in both the
// base rules scope and each per-level scope; when several are active in one scope
// they run in a fixed, hardcoded order designed to minimize awkward overlap.

// FROZEN wire constants: these values are the `mutator_id` sent in the
// vote-options schema and in vote-call packets. Never reorder or reuse.
enum class MutatorId : uint8_t
{
    Instagib = 0,
    Rails = 1,
    Arena = 2,
    Vampire = 3,
    SuperDrain = 4,
    Armored = 5,
    SuperRail = 6,
    BigCraters = 7,
    FlamingEnemies = 8,
    Gibbing = 9,
    Jetpacks = 10,
    HumansVsBots = 11,
    DelayedSupers = 12,
    WeirdGunGame = 13,
    LowGravity = 14,
    ScoreLimitOverride = 15,
    IdealPlayerCountOverride = 16,
    Skiing = 17,
    Pogo = 18,
    Dodging = 19,
    Crits = 20,
};

struct MutatorOptionChoice
{
    std::string label; // shown in the client UI
    std::string value; // written into the TOML declaration
};

// One option of one mutator. `id` is frozen per mutator (wire value).
struct MutatorOptionInfo
{
    uint8_t id = 0;
    std::string name;  // TOML key
    std::string label; // shown in the client UI
    MutatorOptionType type = MutatorOptionType::Bool;

    // Only the member matching `type` is meaningful.
    bool default_bool = false;
    uint8_t default_choice = 0; // index into `choices`
    int32_t default_int = 0;
    float default_float = 0.0f;
    std::string default_string;

    std::vector<MutatorOptionChoice> choices; // Choice only
};

// A mutator that needs no client-side support at all: it imposes no version
// floor and does not even require an Alpine client.
inline constexpr int MUTATOR_NO_CLIENT_REQUIREMENT = 0;

// What a mutator needs from the game type in order to be applied.
enum class MutatorGametypeReq : uint8_t
{
    Any,         // every game type
    TeamOnly,
    GunGameOnly,
    HasScoreLimit, // excludes the types scored without a numeric limit
    BotsSupported, // excludes the types bots cannot play
};

struct MutatorInfo
{
    MutatorId id = MutatorId::Instagib;
    std::string name;  // canonical, matches the TOML `name` key
    std::string label; // shown in the printed rules and the client UI

    // Minimum Alpine Faction MINOR version a client needs in order to play on a
    // server with this mutator active, or MUTATOR_NO_CLIENT_REQUIREMENT for
    // none.
    int min_client_minor_version = MUTATOR_NO_CLIENT_REQUIREMENT;

    // Game types this mutator can be used in.
    uint32_t valid_gametype_mask = MUTATOR_GAMETYPE_MASK_ANY;

    std::vector<MutatorOptionInfo> options;
};

void mutators_do_patch();
void apply_mutators_from_toml(const toml::array& mutators_arr, AlpineServerConfigRules& rules);
void mutators_level_init_post();
void mutators_do_frame();
void mutators_update_low_gravity();
void mutators_on_multi_shutdown();
int mutators_redirect_item_index(int item_type_index);
bool mutators_should_deny_weapon_switch(int from_weapon, int to_weapon);
void mutators_on_player_frag(rf::Player* killer);
// Reload-on-kill, server only: note that the shot currently being processed has earned its
// firer a refill, for that shot's own fire call to spend once it has decremented the clip. Only
// valid to call while the killing bolt's flight is still resolving, i.e. from the lethal
// transition of damage dealt by the firer's own shot - see mutators.cpp.
void mutators_note_pending_frag_refill();
void mutators_set_no_clip_weapon(int weapon_type);
// The weapon currently forced to no-clip, or -1 when there is none.
int mutators_get_no_clip_weapon();
void mutators_on_pvp_damage(rf::Player* attacker, rf::Player* victim, float effective_damage);
void mutators_on_flame_damage(rf::Player* attacker, rf::Player* victim, int damage_type, float damage);
void mutators_on_flame_victim_damage(rf::Player* victim, int damage_type, float damage);
void mutators_on_item_picked_up(rf::Item* item, rf::Entity* entity);
void mutators_apply_entity_on_fire(rf::Entity* ep, bool on_fire);
// Server-authoritative Flaming Enemies burn state for a player id: the id of the player who set
// them alight, while they are still burning from it. -1 when they are not burning and for a burn
// with no owner, so the result never matches a real player id unless there is an igniter to blame.
// Always -1 on clients and whenever the mutator is not running.
int mutators_player_fire_igniter(uint8_t player_id);
// Player ids are reused: a leaving player's burn state must not be inherited by the id's next owner,
// and the crit mutator's per-player state goes with it.
void mutators_on_player_destroy(rf::Player* player);
bool mutators_skiing_active();
bool mutators_dodging_active();
bool mutators_pogo_active();

// ---------------------------------------------------------------------------
// Critical Hits mutator
// ---------------------------------------------------------------------------

// One trigger pull's crit roll. Nesting-aware: multi_process_remote_weapon_fire calls
// entity_fire_weapon, so a remote non-thrown shot opens two scopes and only the outermost
// may roll - re-rolling in the inner one would double the effective crit rate.
class CritFireScope
{
public:
    explicit CritFireScope(rf::Entity* ep);
    ~CritFireScope();

    CritFireScope(const CritFireScope&) = delete;
    CritFireScope& operator=(const CritFireScope&) = delete;

private:
    // Saved rather than cleared, exactly like SplashWeaponScope: an inner scope restores
    // the outer roll on the way out instead of dropping it.
    bool saved_active_;
    bool saved_crit_;
    bool saved_shot_sent_;
    bool saved_fire_sounded_;
    int saved_shooter_handle_;
    bool owns_ = false;
};

// Publishes whether the projectile whose impact/detonation is being resolved was tagged as
// a crit. Constructed every frame for every live weapon (weapon_move_one), so the ctor must
// stay a pure lookup.
class CritWeaponScope
{
public:
    explicit CritWeaponScope(rf::Weapon* wp);
    ~CritWeaponScope();

    CritWeaponScope(const CritWeaponScope&) = delete;
    CritWeaponScope& operator=(const CritWeaponScope&) = delete;

private:
    bool saved_active_;
    bool saved_crit_;
};

// Tag a projectile the open fire scope rolled a crit for. Called from the weapon_create CALL
// SITES only - the lag-comp ghost must never be tagged.
void crits_on_weapon_created(rf::Weapon* wp, int parent_handle);
// Melee swings create their projectiles frames after the fire event, so they take the
// shooter's pending swing roll instead of the fire scope.
void crits_on_deferred_created(rf::Weapon* wp, int parent_handle);
void crits_on_object_dead(rf::Object* objp);
// The crit detonation sound; returns the blast-radius scale the caller must apply to the
// explosion. Called from every player-attributed explosion_apply_radius_damage call site: two
// are hooked here, the third is driven by weapon.cpp.
float crits_on_explosion(const rf::Vector3* pos, float radius);
// Continuous fire (flamethrower stream, taser, drill): the engine re-enters the fire path on its
// own instead of once per shot, so these roll on a cadence and stamp a crit window. `weapon_on`
// is the raw engine state; which weapons the window applies to is decided inside.
void crits_on_continuous_fire_frame(rf::Player* pp, int weapon_type, bool weapon_on, int delta_ms);
// Feeds the recent-damage ramp the crit chance scales with.
void crits_on_damage_dealt(rf::Player* attacker, float effective_damage);
// Damage multiplier for the PvP damage currently being applied, 1.0 when it is not a crit.
float crits_damage_multiplier(rf::Player* attacker, rf::Player* victim);
// Client side, in-flight telegraph. af_crit_shot marker for a shooter's next projectile.
void crits_on_crit_shot(uint8_t shooter_player_id, uint8_t weapon_type);
// Client side, called after the world scene renders.
void crits_client_render();
// Client side, the local player's own crit-fire reticle flash; drawn with the multiplayer HUD.
void crits_client_render_reticle_flash();

// Registry view. Built lazily because choice lists (e.g. the Rails featured
// weapon) are derived from the loaded weapon/item tables.
const std::vector<MutatorInfo>& mutators_get_registry();
const MutatorInfo* mutators_find_by_id(MutatorId id);
const MutatorInfo* mutators_find_by_name(std::string_view name);

// Highest min_client_minor_version among the mutators these declarations put in
// force, or MUTATOR_NO_CLIENT_REQUIREMENT when none of them needs client-side
// support (including the empty list).
int mutators_min_client_minor_version(const std::vector<MutatorDeclaration>& declarations);

// Declarations -> TOML, the shape apply_mutators_from_toml consumes.
toml::array mutator_declarations_to_toml_array(const std::vector<MutatorDeclaration>& declarations);

// Validate packet-supplied mutator selections against the registry and turn them
// into declarations. Returns an error string on failure, std::nullopt on success.
// `game_type` is the type the vote would actually run under; a mutator not valid for
// it is rejected. Explicit wire selections only - an inherited set must keep crossing
// game types.
std::optional<std::string> mutators_build_declarations_from_vote(
    const std::vector<VoteMutatorInput>& input, rf::NetGameType game_type,
    std::vector<MutatorDeclaration>& out);

// Labels of the declared mutators, joined with ", ". Passing a game type drops the
// declarations it would filter out at application time.
std::string mutators_join_labels(const std::vector<MutatorDeclaration>& declarations,
                                 std::optional<rf::NetGameType> game_type = std::nullopt);

// Labels of the mutators these rules actually put in force, or nullopt when none did.
// Every ManualRulesOverride label line derives from here.
std::optional<std::string> mutators_active_labels_string(const AlpineServerConfigRules& rules);

// The game type a level runs under when nothing names one, in precedence order: its
// rotation entry's type, else Run for a quirks-table run map, else the base type if it
// can host the level, else the filename prefix's type, else the base type.
rf::NetGameType resolve_level_default_game_type(std::string_view level_filename);

// Rules for `game_type` built without inheriting any other game type's fields.
// `mutators` is applied last, in MUTATOR_APPLY_ORDER.
AlpineServerConfigRules build_derived_server_rules(rf::NetGameType game_type,
                                                   const std::vector<MutatorDeclaration>& mutators);

// Rules a level/match vote (or a manual level load) installs. `gametype` falls back
// to resolve_level_default_game_type.
ManualRulesOverride load_vote_rules_override(
    std::string_view level_filename, const std::vector<MutatorDeclaration>& mutators,
    std::optional<rf::NetGameType> gametype);
