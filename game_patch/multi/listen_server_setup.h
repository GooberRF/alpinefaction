#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <toml++/toml.hpp>
#include "listen_server_rules.h"

struct ListenServerBot
{
    std::string name;      // empty: random
    std::string character; // empty: random
    std::string skill = ServerBotConfig{}.skill_preset;
    std::string personality = ServerBotConfig{}.personality_preset;
    int team = -1; // -1 auto, else rf::TEAM_RED or rf::TEAM_BLUE
    toml::table tuning; // personality_overrides, skill_overrides and quirks of a loaded profile
};

// A Host Game setup, held as the scopes of a server config file (-ads format). Only what the host set is stored,
// so every rule left alone follows each level's game type defaults.
class ListenServerSetup
{
public:
    // The scope argument of the rule and mutator functions: a rotation index, or this.
    static constexpr int base_scope = -1;

    std::string server_name() const;
    void set_server_name(std::string_view name);
    std::string password() const;
    void set_password(std::string_view password);
    int max_players() const;
    void set_max_players(int count);

    std::optional<ListenServerRuleValue> own_rule(int scope, const ListenServerRule& rule) const;
    void set_rule(int scope, const ListenServerRule& rule, const ListenServerRuleValue& value);
    void unset_rule(int scope, const ListenServerRule& rule);
    // nullopt: the level decides.
    std::optional<ListenServerRuleValue> effective_rule(int scope, const ListenServerRule& rule) const;
    const AlpineServerConfigRules& effective_rules(int scope) const;
    rf::NetGameType game_type(int scope) const;

    int num_levels() const;
    const std::string& level_filename(int index) const;
    void add_level(std::string_view filename, rf::NetGameType game_type);
    void remove_level(int index);
    void move_level(int from, int to);
    void set_level_game_type(int index, rf::NetGameType game_type);
    // A level's own settings: its rules table less its game type.
    bool level_has_own_settings(int index) const;
    toml::table level_settings(int index) const;
    // Replaces them; the level keeps its game type.
    void set_level_settings(int index, const toml::table& settings);
    bool random_rotation() const;
    void set_random_rotation(bool random);

    // nullopt: not declared in this scope. False: a level switches off a base mutator.
    std::optional<bool> own_mutator(int scope, std::string_view name) const;
    // nullopt removes this scope's declaration. A base scope cannot hold a disabled one.
    void set_mutator(int scope, std::string_view name, std::optional<bool> enabled);
    std::optional<MutatorOptionValue> own_mutator_option(int scope, std::string_view name,
                                                         std::string_view option) const;
    // Declares the mutator in this scope if it is not.
    void set_mutator_option(int scope, std::string_view name, std::string_view option, const MutatorOptionValue& value);
    void unset_mutator_option(int scope, std::string_view name, std::string_view option);
    MutatorOptionValue effective_mutator_option(int scope, std::string_view name, std::string_view option) const;

    // The bots that join at the start (server_bots), each on its own profile.
    std::vector<ListenServerBot>& bots();
    const std::vector<ListenServerBot>& bots() const;
    // Every bot_profiles entry, in order: the bots' own, then a loaded config's profiles beyond server_bots, kept for
    // the bots that join later (Join Bots, sv_bot_add).
    std::vector<ListenServerBot> bot_profiles() const;
    // How many of them are a loaded config's, past the bots' own.
    int num_pool_bots() const;

    // The setup as a config file's root table. Bot profiles are written by the save, which passes their paths.
    toml::table to_config_table(const std::vector<std::string>& bot_profile_paths) const;
    static ListenServerSetup from_config_table(const toml::table& root, const std::vector<toml::table>& profile_tables,
                                               std::vector<std::string>& warnings);

private:
    toml::table& scope_table(int scope);
    const toml::table* scope_table(int scope) const;
    toml::array& scope_mutators(int scope);
    const toml::array* scope_mutators(int scope) const;
    void sync_base_game_type();
    void rules_changed() const;

    toml::table m_root;
    toml::table m_base;
    std::vector<toml::table> m_levels; // each [[levels]] entry: filename and rules
    std::vector<ListenServerBot> m_bots;
    std::vector<ListenServerBot> m_pool_bots;
    mutable std::vector<std::optional<AlpineServerConfigRules>> m_rules_cache;
};

enum class ListenServerStartResult
{
    Started,
    StartedLanOnly, // the tracker could not be reached
    NoLevels,
};

// LAN only is the create_game_lan_only setting, which the setup does not hold.
ListenServerStartResult start_listen_server(const ListenServerSetup& setup);

// A new setup, named and passworded as the last one started in any launch.
ListenServerSetup listen_server_default_setup();

// The game types a level can be played in, by its name.
std::vector<rf::NetGameType> listen_server_game_types_for_level(std::string_view filename);
// Levels installed for any game type, sorted.
std::vector<std::string> listen_server_levels();
// Where a stock level leads a list of levels; every other level ranks past them all.
int listen_server_stock_level_rank(std::string_view filename);

// Saved setups in <RF root>\saved\servers. Names are file names without the extension.
bool listen_server_setup_name_char(char c);
bool listen_server_setup_name_valid(std::string_view name);
std::vector<std::string> listen_server_saved_setups();
bool listen_server_save_setup(const ListenServerSetup& setup, std::string_view name);
// Warnings name what the setup could not take: unknown or dedicated-only keys, missing levels, unknown mutators.
std::optional<ListenServerSetup> listen_server_load_setup(std::string_view name, std::vector<std::string>& warnings);

// True once after a Host Game session ends, until the next session starts; the menus then go back to Host Game. The
// main menu takes it.
bool listen_server_take_return_to_create_game();
void listen_server_on_multi_start();
void listen_server_on_multi_stop();
