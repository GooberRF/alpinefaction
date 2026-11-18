#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>
#include "server_internal.h"

struct MutatorOptionChoice;

enum class ListenServerRuleWidget : uint8_t
{
    Toggle,
    Int,
    Float,
    Choice,
    Text,
};

// Where a Choice row's values come from. The value is the name the config file uses.
enum class ListenServerRuleChoices : uint8_t
{
    None,
    Weapons,
    Characters,
};

// A value in the units and type the config file uses.
using ListenServerRuleValue = MutatorOptionValue;

// One single-value rule a server config can set in its [base] or a [[levels]] rules table.
struct ListenServerRule
{
    const char* key; // path inside the rules table, e.g. "spawn_protection.duration"
    const char* label;
    const char* description;
    ListenServerRuleWidget widget;
    float min = 0.0f;
    float max = 0.0f;
    float step = 0.0f;
    ListenServerRuleChoices choices = ListenServerRuleChoices::None;
    uint32_t game_type_mask = MUTATOR_GAMETYPE_MASK_ANY;
    bool basic = false; // listed first on OPTIONS
    const char* group;
    const char* enabled_by = nullptr; // a Toggle row that has to be on for this one to be read
    // nullopt: the config leaves the value to the level. A root rule's gives its default.
    std::optional<ListenServerRuleValue> (*effective)(const AlpineServerConfigRules& rules);
    bool hidden = false; // not offered by Host Game, which lists it only in a scope that sets it
    // A Float row that sets its enabled_by toggle itself, which is off at this value.
    std::optional<float> off_value = std::nullopt;
    bool root = false; // a key at the top of the config, for the whole server
};

std::span<const ListenServerRule> listen_server_rules();
const ListenServerRule* listen_server_find_rule(std::string_view key);
bool listen_server_rule_applies(const ListenServerRule& rule, rf::NetGameType game_type);
std::vector<MutatorOptionChoice> listen_server_rule_choices(const ListenServerRule& rule);
