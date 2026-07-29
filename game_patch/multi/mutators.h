#pragma once

#include <toml++/toml.hpp>

struct AlpineServerConfigRules;

namespace rf
{
    struct Player;
}

// Mutators quickly reconfigure a set of gameplay rules to produce an alternate
// style of play or modify the experience in some way.
//
// Mutators are applied AFTER the gametype defaults but BEFORE any explicitly-set
// rule keys in the same scope, so manual keys always win. They apply in both the
// base rules scope and each per-level scope; when several are active in one scope
// they run in a fixed, hardcoded order designed to minimize awkward overlap.

void mutators_do_patch();
void apply_mutators_from_toml(const toml::array& mutators_arr, AlpineServerConfigRules& rules);
void mutators_level_init_post();
int mutators_redirect_item_index(int item_type_index);
bool mutators_should_deny_weapon_switch(int from_weapon, int to_weapon);
void mutators_on_player_frag(rf::Player* killer);
void mutators_set_no_clip_weapon(int weapon_type);
