#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <toml++/toml.hpp>
#include "server_internal.h"

struct AlpineServerConfigRules;

namespace rf
{
    struct Entity;
    struct Item;
    struct Player;
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

struct MutatorInfo
{
    MutatorId id = MutatorId::Instagib;
    std::string name;  // canonical, matches the TOML `name` key
    std::string label; // shown in the printed rules and the client UI

    // Minimum Alpine Faction MINOR version a client needs in order to play on a
    // server with this mutator active, or MUTATOR_NO_CLIENT_REQUIREMENT for
    // none.
    int min_client_minor_version = MUTATOR_NO_CLIENT_REQUIREMENT;

    std::vector<MutatorOptionInfo> options;
};

void mutators_do_patch();
void apply_mutators_from_toml(const toml::array& mutators_arr, AlpineServerConfigRules& rules);
void mutators_level_init_post();
void mutators_do_frame();
int mutators_redirect_item_index(int item_type_index);
bool mutators_should_deny_weapon_switch(int from_weapon, int to_weapon);
void mutators_on_player_frag(rf::Player* killer);
void mutators_set_no_clip_weapon(int weapon_type);
void mutators_on_pvp_damage(rf::Player* attacker, rf::Player* victim, float effective_damage);
void mutators_on_flame_damage(rf::Player* attacker, rf::Player* victim, int damage_type, float damage);
void mutators_on_flame_victim_damage(rf::Player* victim, int damage_type, float damage);
void mutators_on_item_picked_up(rf::Item* item, rf::Entity* entity);
void mutators_apply_entity_on_fire(rf::Entity* ep, bool on_fire);

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
std::optional<std::string> mutators_build_declarations_from_vote(
    const std::vector<VoteMutatorInput>& input, std::vector<MutatorDeclaration>& out);

// Human-readable labels of the declared mutators, joined with ", ".
std::string mutators_join_labels(const std::vector<MutatorDeclaration>& declarations);

// The rules `level_filename` would run with if no vote were involved: its
// rotation entry's rules when it is in the rotation, otherwise the base rules —
// in both cases with config-declared mutators stripped.
const AlpineServerConfigRules& vote_natural_rules_for_level(std::string_view level_filename);

// Build the rules a level/match vote should install: the voted level's natural
// rules (above), optionally re-based on a voted game type plus that type's
// defaults, then the voted mutators applied in MUTATOR_APPLY_ORDER.
std::optional<ManualRulesOverride> load_vote_rules_override(
    std::string_view level_filename, const std::vector<MutatorDeclaration>& mutators,
    std::optional<rf::NetGameType> gametype);
