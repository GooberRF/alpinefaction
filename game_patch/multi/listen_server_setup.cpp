#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <format>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <common/utils/os-utils.h>
#include <common/utils/string-utils.h>
#include <xlog/xlog.h>
#include "listen_server_setup.h"
#include "dedi_cfg.h"
#include "multi.h"
#include "mutators.h"
#include "saved_info.h"
#include "server.h"
#include "../misc/alpine_settings.h"
#include "../rf/file/file.h"
#include "../rf/multi.h"
#include "../rf/gameseq.h"
#include "../rf/localize.h"

namespace fs = std::filesystem;

namespace
{
// Each profile is a file to parse on load and to write on every save.
constexpr size_t max_setup_bot_profiles = 64;
// A setup that leaves geo_limit out starts with the most craters, where the parser defaults to 64.
constexpr int64_t default_geo_limit = 128;

// Rule keys without a rule row. A loaded setup keeps the list-type ones Host Game does not edit, so they are
// saved again and applied.
constexpr std::string_view kept_rule_keys[] = {
    "game_type",
    "mutators",
    "spawn_loadout",
    "spawn_loadout_blue",
    "weapon_stay_exemptions",
    "item_replacements",
    "item_respawn_time_overrides",
    "delayed_items",
    "gg_tiers",
    "welcome_message",
    "level_message",
};

constexpr std::string_view bot_tuning_keys[] = {"personality_overrides", "skill_overrides", "quirks"};

bool g_return_to_create_game = false;

// Windows opens these devices for a file of the name, whatever the extension.
bool is_reserved_device_name(std::string_view name)
{
    constexpr std::string_view devices[] = {"con", "prn", "aux", "nul"};
    if (std::any_of(std::begin(devices), std::end(devices),
                    [&](std::string_view device) { return string_iequals(name, device); })) {
        return true;
    }
    return name.size() == 4 && (string_istarts_with(name, "com") || string_istarts_with(name, "lpt"))
        && name[3] >= '1' && name[3] <= '9';
}

const AlpineServerConfig& default_config()
{
    static const AlpineServerConfig cfg;
    return cfg;
}

// What a key an overtime table leaves out reads as: the parser starts each table from the defaults.
const AlpineServerConfigRules& missing_overtime_key_rules()
{
    static const AlpineServerConfigRules rules;
    return rules;
}

struct RuleKey
{
    std::string_view table; // empty for a key at the top of the rules table
    std::string_view leaf;
};

RuleKey split_rule_key(std::string_view key)
{
    const size_t dot = key.find('.');
    if (dot == std::string_view::npos) {
        return {{}, key};
    }
    return {key.substr(0, dot), key.substr(dot + 1)};
}

bool is_known_rule_key(std::string_view key)
{
    if (std::find(std::begin(kept_rule_keys), std::end(kept_rule_keys), key) != std::end(kept_rule_keys)) {
        return true;
    }
    const auto rules = listen_server_rules();
    return std::any_of(rules.begin(), rules.end(), [&](const ListenServerRule& rule) {
        const RuleKey path = split_rule_key(rule.key);
        return !rule.root && (path.table.empty() ? path.leaf : path.table) == key;
    });
}

toml::table& child_table(toml::table& parent, std::string_view key)
{
    if (toml::table* child = parent.get_as<toml::table>(key)) {
        return *child;
    }
    return *parent.insert_or_assign(key, toml::table{}).first->second.as_table();
}

void copy_node(toml::table& dst, std::string_view key, const toml::node& node)
{
    node.visit([&](const auto& concrete) { dst.insert_or_assign(key, concrete); });
}

// What a level reads for overtime without a table of its own.
AlpineServerConfigRules inherited_overtime_rules(const toml::table& base, toml::table level)
{
    child_table(level, "rules").erase("overtime");
    return build_config_scope_rules(base, &level);
}

// A level's overtime table replaces the inherited one whole, so a key it leaves out gets the inherited value where
// leaving it out reads otherwise. With copy_shown, so does each key of a row Host Game lists, whatever it reads.
void fill_overtime_table(toml::table& overtime, const AlpineServerConfigRules& inherited, bool copy_shown)
{
    for (const ListenServerRule& row : listen_server_rules()) {
        const RuleKey path = split_rule_key(row.key);
        if (path.table != "overtime" || overtime.contains(path.leaf)) {
            continue;
        }
        const auto value = row.effective(inherited);
        if (value && ((copy_shown && !row.hidden) || value != row.effective(missing_overtime_key_rules()))) {
            toml_insert_option_value(overtime, path.leaf, *value);
        }
    }
}

// These lists add up across layers, as the parser applies both layers' entries.
constexpr std::string_view accumulated_list_keys[] = {
    "mutators",
    "spawn_loadout",
    "spawn_loadout_blue",
    "weapon_stay_exemptions",
    "item_replacements",
    "item_respawn_time_overrides",
    "delayed_items",
};

// A later layer's keys win, as the parser reads it second. The parser skips a value key in a layer whose table is not
// enabled, which the merge keeps if a later layer enables the table. The parser reads an overtime table afresh, so a
// later one replaces it whole, and rebuilds dynamic_respawn_items from each spawn_selection table, so a later one
// without the list drops it.
void merge_tables(toml::table& dst, const toml::table& src)
{
    for (auto&& [key, node] : src) {
        toml::node* existing = dst.get(key.str());
        if (existing && existing->is_table() && node.is_table() && key.str() != "overtime") {
            merge_tables(*existing->as_table(), *node.as_table());
            if (key.str() == "spawn_selection" && !node.as_table()->contains("dynamic_respawn_items")) {
                existing->as_table()->erase("dynamic_respawn_items");
            }
        }
        else if (existing && existing->is_array() && node.is_array()
                 && std::find(std::begin(accumulated_list_keys), std::end(accumulated_list_keys), key.str())
                        != std::end(accumulated_list_keys)) {
            for (const toml::node& entry : *node.as_array()) {
                entry.visit([&](const auto& concrete) { existing->as_array()->push_back(concrete); });
            }
        }
        else {
            copy_node(dst, key.str(), node);
        }
    }
}

std::optional<ListenServerRuleValue> rule_value(const toml::node& node, ListenServerRuleWidget widget)
{
    switch (widget) {
        case ListenServerRuleWidget::Toggle:
            return toml_option_value(node, MutatorOptionType::Bool);
        case ListenServerRuleWidget::Int:
            return toml_option_value(node, MutatorOptionType::Int);
        case ListenServerRuleWidget::Float:
            return toml_option_value(node, MutatorOptionType::Float);
        case ListenServerRuleWidget::Choice:
        case ListenServerRuleWidget::Text:
            return toml_option_value(node, MutatorOptionType::String);
    }
    return std::nullopt;
}

const MutatorOptionInfo* find_mutator_option(const MutatorInfo& info, std::string_view option)
{
    const auto it = std::find_if(info.options.begin(), info.options.end(),
        [&](const MutatorOptionInfo& opt) { return opt.name == option; });
    return it != info.options.end() ? &*it : nullptr;
}

// The declaration that wins in this array: the last one naming the mutator.
template<typename Array>
auto* last_mutator_entry(Array& arr, std::string_view canonical_name)
{
    decltype(arr.back().as_table()) found = nullptr;
    for (auto& entry : arr) {
        if (mutator_entry_names(entry, canonical_name)) {
            found = entry.as_table();
        }
    }
    return found;
}

toml::table bot_profile_table(const ListenServerBot& bot)
{
    toml::table tbl = bot.tuning;
    if (!bot.name.empty()) {
        tbl.insert_or_assign("player_name", bot.name);
    }
    if (!bot.character.empty()) {
        tbl.insert_or_assign("mp_character", bot.character);
    }
    tbl.insert_or_assign("personality_preset", bot.personality);
    tbl.insert_or_assign("skill_preset", bot.skill);
    return tbl;
}

ListenServerBot bot_from_profile(const toml::table& profile, std::string_view source,
                                 std::vector<std::string>& warnings)
{
    ListenServerBot bot;
    for (auto&& [key, node] : profile) {
        const std::string_view name = key.str();
        if (name == "player_name") {
            bot.name = node.value_or<std::string>("");
        }
        else if (name == "mp_character") {
            bot.character = node.value_or<std::string>("");
        }
        else if (name == "personality_preset") {
            if (auto v = node.value<std::string>()) {
                bot.personality = *v;
            }
        }
        else if (name == "skill_preset") {
            if (auto v = node.value<std::string>()) {
                bot.skill = *v;
            }
        }
        else if (std::find(std::begin(bot_tuning_keys), std::end(bot_tuning_keys), name)
                 != std::end(bot_tuning_keys)) {
            copy_node(bot.tuning, name, node);
        }
        else {
            warnings.push_back(std::format("Bot profile {}: '{}' is not a bot setting and was ignored.", source, name));
        }
    }
    return bot;
}

// One rules scope of a loaded config, as Host Game keeps it: [base] or a level's rules, with the scope's
// [rules] table folded in and the keys Host Game cannot take left out.
toml::table load_rule_scope(const toml::table& src, std::string_view where, std::vector<std::string>& warnings)
{
    toml::table merged;
    for (auto&& [key, node] : src) {
        const std::string_view name = key.str();
        if (name != "rules" && name != "filename" && name != "rules_presets") {
            copy_node(merged, name, node);
        }
    }
    if (const toml::table* rules = src.get_as<toml::table>("rules")) {
        merge_tables(merged, *rules);
    }

    toml::table scope;
    for (auto&& [key, node] : merged) {
        const std::string_view name = key.str();
        if (!is_known_rule_key(name)) {
            warnings.push_back(std::format("{}: '{}' is not a Host Game setting and was ignored.", where, name));
            continue;
        }
        if (name != "mutators") {
            copy_node(scope, name, node);
            continue;
        }
        const toml::array* entries = node.as_array();
        if (!entries) {
            continue;
        }
        toml::array kept;
        for (const toml::node& entry : *entries) {
            const toml::table* tbl = entry.as_table();
            const std::string mutator = tbl ? (*tbl)["name"].value_or<std::string>("") : std::string{};
            if (!tbl || !mutators_find_by_name(mutator)) {
                warnings.push_back(std::format("{}: unknown mutator '{}' was left out.", where, mutator));
                continue;
            }
            kept.push_back(*tbl);
        }
        if (!kept.empty()) {
            scope.insert_or_assign("mutators", std::move(kept));
        }
    }
    // A value the server would skip, such as nan, would otherwise only be reported at START.
    for (const ListenServerRule& rule : listen_server_rules()) {
        const RuleKey path = split_rule_key(rule.key);
        toml::table* parent = path.table.empty() ? &scope : scope.get_as<toml::table>(path.table);
        const toml::node* node = parent ? parent->get(path.leaf) : nullptr;
        if (node && !rule_value(*node, rule.widget)) {
            warnings.push_back(std::format("{}: the value of '{}' cannot be used and was ignored.", where, rule.key));
            parent->erase(path.leaf);
        }
    }
    return scope;
}

fs::path servers_dir(bool create)
{
    const std::string saved = saved_dir_path(create);
    if (saved.empty()) {
        return {};
    }
    fs::path dir = fs::path{saved} / "servers";
    if (create) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) {
            xlog::error("[ListenServer] Failed to create {}: {}", dir.string(), ec.message());
            return {};
        }
    }
    return dir;
}

// A path part as text, or none when it holds a character outside ASCII: narrowing such a part throws where the code
// page has no mapping for it, and no setup or profile name has one.
std::optional<std::string> ascii_path_text(const fs::path& part)
{
    const std::wstring wide = part.wstring();
    std::string text;
    for (const wchar_t c : wide) {
        if (c >= 0x80) {
            return std::nullopt;
        }
        text += static_cast<char>(c);
    }
    return text;
}

bool write_toml(const fs::path& path, const toml::table& tbl)
{
    std::ostringstream out;
    out << tbl << '\n';
    try {
        write_file_replacing(path.string(), out.str());
    }
    catch (const std::exception& e) {
        xlog::error("[ListenServer] Failed to write {}: {}", path.string(), e.what());
        return false;
    }
    return true;
}

bool save_setup(const ListenServerSetup& setup, std::string_view name)
{
    const fs::path dir = servers_dir(true);
    if (dir.empty()) {
        return false;
    }
    const std::string bots_dir_name = std::format("{}.bots", name);
    const fs::path bots_dir = dir / bots_dir_name;
    const std::vector<ListenServerBot> profiles = setup.bot_profiles();
    std::error_code ec;
    if (!profiles.empty()) {
        fs::create_directories(bots_dir, ec);
        if (ec) {
            xlog::error("[ListenServer] Failed to create {}: {}", bots_dir.string(), ec.message());
            return false;
        }
    }

    std::vector<std::string> profile_paths;
    std::set<std::string> written;
    for (size_t i = 0; i < profiles.size(); ++i) {
        std::string file = std::format("{:02}.toml", i);
        if (!write_toml(bots_dir / file, bot_profile_table(profiles[i]))) {
            return false;
        }
        profile_paths.push_back(std::format("{}/{}", bots_dir_name, file));
        written.insert(std::move(file));
    }

    if (!write_toml(dir / std::format("{}.toml", name), setup.to_config_table(profile_paths))) {
        return false;
    }
    // Only once the config no longer names them.
    if (fs::is_directory(bots_dir, ec)) {
        for (fs::directory_iterator it{bots_dir, ec}, end; !ec && it != end; it.increment(ec)) {
            const std::optional<std::string> file = ascii_path_text(it->path().filename());
            const std::optional<std::string> ext = ascii_path_text(it->path().extension());
            if (file && ext && string_iequals(*ext, ".toml") && !written.contains(*file)) {
                std::error_code remove_ec;
                fs::remove(it->path(), remove_ec);
            }
        }
        if (profiles.empty()) {
            fs::remove(bots_dir, ec); // only goes when nothing else is in it
        }
    }
    return true;
}

const toml::node* root_key(const toml::table& root, std::string_view key)
{
    if (const toml::node* node = root.get(key)) {
        return node;
    }
    const toml::table* nested = root.get_as<toml::table>("root");
    return nested ? nested->get(key) : nullptr;
}

std::optional<ListenServerSetup> load_setup(std::string_view name, std::vector<std::string>& warnings)
{
    const fs::path dir = servers_dir(false);
    if (dir.empty()) {
        return std::nullopt;
    }
    const fs::path file = dir / std::format("{}.toml", name);
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        return std::nullopt;
    }
    toml::table root;
    try {
        root = toml::parse_file(file.string());
    }
    catch (const toml::parse_error& err) {
        warnings.push_back(std::format("{} could not be read: {}", file.filename().string(), err.description()));
        return std::nullopt;
    }

    std::vector<toml::table> profiles;
    if (const toml::node* node = root_key(root, "bot_profiles"); node && node->is_array()) {
        const toml::array& entries = *node->as_array();
        if (entries.size() > max_setup_bot_profiles) {
            warnings.push_back(std::format("Only the first {} bot_profiles entries were read; {} more were left out.",
                                           max_setup_bot_profiles, entries.size() - max_setup_bot_profiles));
        }
        for (size_t i = 0; i < std::min(entries.size(), max_setup_bot_profiles); ++i) {
            const std::optional<std::string> path = entries[i].value<std::string>();
            if (!path) {
                warnings.push_back("A bot_profiles entry is not a file name; using defaults.");
                profiles.emplace_back();
                continue;
            }
            try {
                profiles.push_back(toml::parse_file((file.parent_path() / *path).string()));
            }
            catch (const toml::parse_error& err) {
                warnings.push_back(std::format("Bot profile {} could not be read: {}", *path, err.description()));
                profiles.emplace_back();
            }
            // A path the code page cannot convert, in a hand-edited config.
            catch (const std::exception&) {
                warnings.push_back(std::format("Bot profile {} could not be read.", *path));
                profiles.emplace_back();
            }
        }
    }
    return ListenServerSetup::from_config_table(root, profiles, warnings);
}

} // namespace

std::string ListenServerSetup::server_name() const
{
    // Stock's name for a setup with none; set_server_name already replaces a blank one.
    return m_root["server_name"].value<std::string>().value_or(rf::strings::unnamed_server);
}

void ListenServerSetup::set_server_name(std::string_view name)
{
    m_root.insert_or_assign("server_name", std::string{trim(name).empty() ? rf::strings::unnamed_server : name});
}

std::string ListenServerSetup::password() const
{
    return m_root["password"].value<std::string>().value_or(default_config().password);
}

void ListenServerSetup::set_password(std::string_view password)
{
    AlpineServerConfig cfg;
    cfg.set_password(password);
    m_root.insert_or_assign("password", cfg.password);
}

int ListenServerSetup::max_players() const
{
    return m_root["max_players"].value<int>().value_or(default_config().max_players);
}

void ListenServerSetup::set_max_players(int count)
{
    AlpineServerConfig cfg;
    cfg.set_max_players(count);
    m_root.insert_or_assign("max_players", cfg.max_players);
}

toml::table& ListenServerSetup::scope_table(int scope)
{
    return scope == base_scope ? m_base : child_table(m_levels.at(scope), "rules");
}

const toml::table* ListenServerSetup::scope_table(int scope) const
{
    return scope == base_scope ? &m_base : m_levels.at(scope).get_as<toml::table>("rules");
}

toml::array& ListenServerSetup::scope_mutators(int scope)
{
    toml::table& tbl = scope_table(scope);
    if (toml::array* arr = tbl.get_as<toml::array>("mutators")) {
        return *arr;
    }
    return *tbl.insert_or_assign("mutators", toml::array{}).first->second.as_array();
}

const toml::array* ListenServerSetup::scope_mutators(int scope) const
{
    const toml::table* tbl = scope_table(scope);
    return tbl ? tbl->get_as<toml::array>("mutators") : nullptr;
}

void ListenServerSetup::rules_changed() const
{
    m_rules_cache.assign(m_levels.size() + 1, std::nullopt);
}

std::optional<ListenServerRuleValue> ListenServerSetup::own_rule(int scope, const ListenServerRule& rule) const
{
    const toml::table* tbl = rule.root ? &m_root : scope_table(scope);
    const RuleKey path = split_rule_key(rule.key);
    if (tbl && !path.table.empty()) {
        tbl = tbl->get_as<toml::table>(path.table);
    }
    const toml::node* node = tbl ? tbl->get(path.leaf) : nullptr;
    return node ? rule_value(*node, rule.widget) : std::nullopt;
}

void ListenServerSetup::set_rule(int scope, const ListenServerRule& rule, const ListenServerRuleValue& value)
{
    if (rule.root) {
        toml_insert_option_value(m_root, rule.key, value);
        return;
    }
    const RuleKey path = split_rule_key(rule.key);
    if (const ListenServerRule* gate = rule.off_value ? listen_server_find_rule(rule.enabled_by) : nullptr) {
        // A level writes the toggle off, not left out, so it overrides a game type or base that turns it on. The base
        // leaves the stock value to each level's game type, as PIT, WO and SCAV raise it, unless its own game type
        // (the first level's) raises it too.
        const float* number = std::get_if<float>(&value);
        const bool on = !number || *number != *rule.off_value;
        if (!on && scope == base_scope) {
            unset_rule(scope, rule);
            if (rule.effective(effective_rules(scope)) == value) {
                return;
            }
        }
        set_rule(scope, *gate, on);
        if (!on) {
            if (toml::table* parent = scope_table(scope).get_as<toml::table>(path.table)) {
                parent->erase(path.leaf);
            }
            rules_changed();
            return;
        }
    }
    toml::table& tbl = scope_table(scope);
    if (scope != base_scope && path.table == "overtime") {
        // A new table takes the inherited values of the rows Host Game lists. A hidden row's value, or a key an
        // existing table lacks, is copied only where leaving it out reads otherwise, so the row is not listed for a
        // value the level did not set.
        const AlpineServerConfigRules inherited = inherited_overtime_rules(m_base, m_levels.at(scope));
        const bool created = !tbl.contains("overtime");
        fill_overtime_table(child_table(tbl, "overtime"), inherited, created);
    }
    if (scope != base_scope && path.table == "spawn_selection") {
        // Any spawn_selection table rebuilds dynamic_respawn_items from its own list.
        toml::table& selection = child_table(tbl, "spawn_selection");
        const toml::table* base_selection = m_base.get_as<toml::table>("spawn_selection");
        const toml::array* items =
            base_selection ? base_selection->get_as<toml::array>("dynamic_respawn_items") : nullptr;
        if (items && !selection.contains("dynamic_respawn_items")) {
            selection.insert_or_assign("dynamic_respawn_items", *items);
        }
    }
    toml::table& parent = path.table.empty() ? tbl : child_table(tbl, path.table);
    toml_insert_option_value(parent, path.leaf, value);
    rules_changed();
}

void ListenServerSetup::unset_rule(int scope, const ListenServerRule& rule)
{
    if (rule.root) {
        m_root.erase(rule.key);
        return;
    }
    if (const ListenServerRule* gate = rule.off_value ? listen_server_find_rule(rule.enabled_by) : nullptr) {
        unset_rule(scope, *gate);
    }
    const RuleKey path = split_rule_key(rule.key);
    toml::table& tbl = scope_table(scope);
    if (path.table.empty()) {
        tbl.erase(path.leaf);
    }
    else if (scope != base_scope && path.table == "overtime") {
        // Only a level without an overtime table inherits one, so the table goes, unless another of its keys differs
        // from the inherited value. That table stays, with this key back on the inherited value.
        if (toml::table* overtime = tbl.get_as<toml::table>(path.table)) {
            const AlpineServerConfigRules inherited = inherited_overtime_rules(m_base, m_levels.at(scope));
            const bool keep = std::any_of(overtime->begin(), overtime->end(), [&](const auto& entry) {
                const std::string_view leaf = entry.first.str();
                if (leaf == path.leaf) {
                    return false;
                }
                const ListenServerRule* row = listen_server_find_rule(std::format("{}.{}", path.table, leaf));
                return !row || rule_value(entry.second, row->widget) != row->effective(inherited);
            });
            const auto value = rule.effective(inherited);
            if (!keep) {
                tbl.erase(path.table);
            }
            else {
                if (value && value != rule.effective(missing_overtime_key_rules())) {
                    toml_insert_option_value(*overtime, path.leaf, *value);
                }
                else {
                    overtime->erase(path.leaf);
                }
                fill_overtime_table(*overtime, inherited, false);
            }
        }
    }
    else if (toml::table* parent = tbl.get_as<toml::table>(path.table)) {
        parent->erase(path.leaf);
        if (scope != base_scope && path.table == "spawn_selection" && parent->size() == 1) {
            // Left with only the base's list set_rule copied, the level inherits the same without the table.
            const toml::table* base_selection = m_base.get_as<toml::table>(path.table);
            const toml::array* items = parent->get_as<toml::array>("dynamic_respawn_items");
            const toml::array* base_items =
                base_selection ? base_selection->get_as<toml::array>("dynamic_respawn_items") : nullptr;
            if (items && base_items && *items == *base_items) {
                parent->clear();
            }
        }
        if (parent->empty()) {
            tbl.erase(path.table);
        }
    }
    rules_changed();
}

std::optional<ListenServerRuleValue> ListenServerSetup::effective_rule(int scope, const ListenServerRule& rule) const
{
    if (rule.root) {
        if (auto own = own_rule(scope, rule)) {
            return own;
        }
    }
    return rule.effective(effective_rules(scope));
}

const AlpineServerConfigRules& ListenServerSetup::effective_rules(int scope) const
{
    if (m_rules_cache.size() != m_levels.size() + 1) {
        rules_changed();
    }
    std::optional<AlpineServerConfigRules>& cached = m_rules_cache.at(scope + 1);
    if (!cached) {
        cached = build_config_scope_rules(m_base, scope == base_scope ? nullptr : &m_levels.at(scope));
    }
    return *cached;
}

rf::NetGameType ListenServerSetup::game_type(int scope) const
{
    return effective_rules(scope).game_type;
}

int ListenServerSetup::num_levels() const
{
    return static_cast<int>(m_levels.size());
}

const std::string& ListenServerSetup::level_filename(int index) const
{
    return **m_levels.at(index).get_as<std::string>("filename");
}

void ListenServerSetup::add_level(std::string_view filename, rf::NetGameType game_type)
{
    toml::table level;
    level.insert_or_assign("filename", normalize_level_filename(filename));
    level.insert_or_assign("rules", toml::table{{"game_type", std::string{multi_game_type_name_short(game_type)}}});
    m_levels.push_back(std::move(level));
    sync_base_game_type();
}

void ListenServerSetup::remove_level(int index)
{
    if (index < 0 || index >= num_levels()) {
        return;
    }
    m_levels.erase(m_levels.begin() + index);
    sync_base_game_type();
}

void ListenServerSetup::move_level(int from, int to)
{
    toml::table level = std::move(m_levels.at(from));
    m_levels.erase(m_levels.begin() + from);
    m_levels.insert(m_levels.begin() + std::clamp(to, 0, num_levels()), std::move(level));
    sync_base_game_type();
}

void ListenServerSetup::set_level_game_type(int index, rf::NetGameType game_type)
{
    child_table(m_levels.at(index), "rules")
        .insert_or_assign("game_type", std::string{multi_game_type_name_short(game_type)});
    sync_base_game_type();
}

bool ListenServerSetup::level_has_own_settings(int index) const
{
    const toml::table* rules = m_levels.at(index).get_as<toml::table>("rules");
    return rules && std::any_of(rules->begin(), rules->end(), [](const auto& entry) {
        return entry.first.str() != "game_type";
    });
}

toml::table ListenServerSetup::level_settings(int index) const
{
    toml::table settings;
    if (const toml::table* rules = m_levels.at(index).get_as<toml::table>("rules")) {
        settings = *rules;
        settings.erase("game_type");
    }
    return settings;
}

void ListenServerSetup::set_level_settings(int index, const toml::table& settings)
{
    toml::table& rules = child_table(m_levels.at(index), "rules");
    const toml::node* type = rules.get("game_type");
    toml::table replaced = settings;
    if (type) {
        copy_node(replaced, "game_type", *type);
    }
    rules = std::move(replaced);
    rules_changed();
}

// The first entry's game type, so the base rules show what the session starts with and a level loaded outside the
// rotation resolves as it would on a dedicated server.
void ListenServerSetup::sync_base_game_type()
{
    if (!m_levels.empty()) {
        if (auto type = m_levels.front()["rules"]["game_type"].value<std::string>()) {
            m_base.insert_or_assign("game_type", *type);
        }
    }
    rules_changed();
}

bool ListenServerSetup::random_rotation() const
{
    return m_root["dynamic_rotation"].value<bool>().value_or(default_config().dynamic_rotation);
}

void ListenServerSetup::set_random_rotation(bool random)
{
    m_root.insert_or_assign("dynamic_rotation", random);
}

std::optional<bool> ListenServerSetup::own_mutator(int scope, std::string_view name) const
{
    const MutatorInfo* info = mutators_find_by_name(name);
    const toml::array* arr = scope_mutators(scope);
    const toml::table* entry = info && arr ? last_mutator_entry(*arr, info->name) : nullptr;
    if (!entry) {
        return std::nullopt;
    }
    return (*entry)["enabled"].value_or(true);
}

void ListenServerSetup::set_mutator(int scope, std::string_view name, std::optional<bool> enabled)
{
    const MutatorInfo* info = mutators_find_by_name(name);
    if (!info) {
        return;
    }
    toml::array& arr = scope_mutators(scope);
    toml::table entry{{"name", info->name}};
    const toml::table* existing = last_mutator_entry(arr, info->name);
    if (!existing && scope != base_scope) {
        // A level's declaration replaces the base one whole, so it starts from the base options.
        if (const toml::array* base = std::as_const(*this).scope_mutators(base_scope)) {
            existing = last_mutator_entry(*base, info->name);
        }
    }
    if (existing && enabled.value_or(false)) {
        entry = *existing;
        entry.erase("enabled");
    }
    for (auto it = arr.begin(); it != arr.end();) {
        it = mutator_entry_names(*it, info->name) ? arr.erase(it) : it + 1;
    }
    if (enabled == true) {
        arr.push_back(std::move(entry));
    }
    else if (enabled == false && scope != base_scope) {
        arr.push_back(toml::table{{"name", info->name}, {"enabled", false}});
    }
    if (arr.empty()) {
        scope_table(scope).erase("mutators");
    }
    rules_changed();
}

std::optional<MutatorOptionValue> ListenServerSetup::own_mutator_option(int scope, std::string_view name,
                                                                        std::string_view option) const
{
    const MutatorInfo* info = mutators_find_by_name(name);
    const MutatorOptionInfo* opt = info ? find_mutator_option(*info, option) : nullptr;
    const toml::array* arr = scope_mutators(scope);
    const toml::table* entry = opt && arr ? last_mutator_entry(*arr, info->name) : nullptr;
    const toml::node* node = entry ? entry->get(option) : nullptr;
    return node ? toml_option_value(*node, opt->type) : std::nullopt;
}

void ListenServerSetup::set_mutator_option(int scope, std::string_view name, std::string_view option,
                                           const MutatorOptionValue& value)
{
    const MutatorInfo* info = mutators_find_by_name(name);
    if (!info || !find_mutator_option(*info, option)) {
        return;
    }
    set_mutator(scope, name, true);
    toml_insert_option_value(*last_mutator_entry(scope_mutators(scope), info->name), option, value);
    rules_changed();
}

void ListenServerSetup::unset_mutator_option(int scope, std::string_view name, std::string_view option)
{
    const MutatorInfo* info = mutators_find_by_name(name);
    toml::array* arr = scope_table(scope).get_as<toml::array>("mutators");
    if (toml::table* entry = info && arr ? last_mutator_entry(*arr, info->name) : nullptr) {
        entry->erase(option);
        rules_changed();
    }
}

MutatorOptionValue ListenServerSetup::effective_mutator_option(int scope, std::string_view name,
                                                               std::string_view option) const
{
    const MutatorInfo* info = mutators_find_by_name(name);
    const MutatorOptionInfo* opt = info ? find_mutator_option(*info, option) : nullptr;
    if (!opt) {
        return {};
    }
    for (const MutatorDeclaration& decl : effective_rules(scope).mutators.declarations) {
        if (string_iequals(decl.name, info->name)) {
            if (const auto it = decl.options.find(opt->name); it != decl.options.end()) {
                return it->second;
            }
        }
    }
    return mutator_option_default(*opt);
}

std::vector<ListenServerBot>& ListenServerSetup::bots()
{
    return m_bots;
}

const std::vector<ListenServerBot>& ListenServerSetup::bots() const
{
    return m_bots;
}

std::vector<ListenServerBot> ListenServerSetup::bot_profiles() const
{
    std::vector<ListenServerBot> profiles = m_bots;
    profiles.insert(profiles.end(), m_pool_bots.begin(), m_pool_bots.end());
    return profiles;
}

int ListenServerSetup::num_pool_bots() const
{
    return static_cast<int>(m_pool_bots.size());
}

toml::table ListenServerSetup::to_config_table(const std::vector<std::string>& bot_profile_paths) const
{
    toml::table root = m_root;
    root.insert_or_assign("server_bots", static_cast<int64_t>(m_bots.size()));
    if (!m_bots.empty() || !m_pool_bots.empty()) {
        toml::array teams;
        for (const ListenServerBot& bot : bot_profiles()) {
            teams.push_back(std::string{bot_team_config_name(bot.team)});
        }
        root.insert_or_assign("bot_teams", std::move(teams));
    }
    if (!bot_profile_paths.empty()) {
        toml::array profiles;
        for (const std::string& path : bot_profile_paths) {
            profiles.push_back(path);
        }
        root.insert_or_assign("bot_profiles", std::move(profiles));
    }
    root.insert_or_assign("base", m_base);
    toml::array levels;
    for (const toml::table& level : m_levels) {
        levels.push_back(level);
    }
    root.insert_or_assign("levels", std::move(levels));
    return root;
}

ListenServerSetup ListenServerSetup::from_config_table(const toml::table& root,
                                                       const std::vector<toml::table>& profile_tables,
                                                       std::vector<std::string>& warnings)
{
    ListenServerSetup setup;
    std::optional<int> server_bots;
    std::vector<int> teams;

    std::vector<std::pair<std::string_view, const toml::node*>> entries;
    for (auto&& [key, node] : root) {
        if (key.str() == "root" && node.is_table()) {
            for (auto&& [nested_key, nested_node] : *node.as_table()) {
                entries.emplace_back(nested_key.str(), &nested_node);
            }
        }
        else {
            entries.emplace_back(key.str(), &node);
        }
    }

    for (const auto& [key, node] : entries) {
        if (key == "server_name" && node->is_string()) {
            if (trim(**node->as_string()).empty()) {
                warnings.push_back(std::format("'{}' is blank; using '{}'.", key, rf::strings::unnamed_server));
            }
            setup.set_server_name(**node->as_string());
        }
        else if (key == "password" && node->is_string()) {
            setup.set_password(**node->as_string());
        }
        else if (key == "max_players" || key == "server_bots") {
            // Read as the server reads them, which skips a value out of range or of another type.
            if (const std::optional<int> count = node->value<int>(); !count) {
                warnings.push_back(std::format("The value of '{}' cannot be used and was ignored.", key));
            }
            else if (key == "max_players") {
                setup.set_max_players(*count);
            }
            else {
                server_bots = count;
            }
        }
        else if (key == "dynamic_rotation") {
            if (const std::optional<bool> random = node->value<bool>()) {
                setup.set_random_rotation(*random);
            }
            else {
                warnings.push_back(std::format("The value of '{}' cannot be used and was ignored.", key));
            }
        }
        else if (key == "bot_teams" && node->is_array()) {
            for (const toml::node& entry : *node->as_array()) {
                const std::string name = entry.value_or<std::string>("");
                const auto team = bot_team_from_config_name(name);
                if (!team) {
                    warnings.push_back(
                        std::format("Bot team '{}' is not auto, red or blue; the bot picks its team.", name));
                }
                teams.push_back(team.value_or(-1));
            }
        }
        else if (key == "bot_profiles" || key == "ads_version") {
            // Profiles arrive parsed; the version is not used.
        }
        else if (key == "base" && node->is_table()) {
            setup.m_base = load_rule_scope(*node->as_table(), "[base]", warnings);
        }
        else if (key == "levels" && node->is_array()) {
            for (const toml::node& entry : *node->as_array()) {
                const toml::table* level = entry.as_table();
                if (!level) {
                    continue;
                }
                const std::string filename = normalize_level_filename((*level)["filename"].value_or<std::string>(""));
                rf::File file;
                if (filename.empty() || !file.find(filename.c_str())) {
                    warnings.push_back(std::format("Level '{}' is not installed and was left out.", filename));
                    continue;
                }
                toml::table kept;
                kept.insert_or_assign("filename", filename);
                kept.insert_or_assign("rules", load_rule_scope(*level, std::format("Level {}", filename), warnings));
                setup.m_levels.push_back(std::move(kept));
            }
        }
        else if (const ListenServerRule* rule = listen_server_find_rule(key); rule && rule->root) {
            if (const std::optional<ListenServerRuleValue> value = rule_value(*node, rule->widget)) {
                setup.set_rule(base_scope, *rule, *value);
            }
            else {
                warnings.push_back(std::format("The value of '{}' cannot be used and was ignored.", key));
            }
        }
        else if (key == "server_name" || key == "password" || key == "bot_teams" || key == "base" || key == "levels") {
            // A setting, but of another type.
            warnings.push_back(std::format("The value of '{}' cannot be used and was ignored.", key));
        }
        else {
            warnings.push_back(std::format("'{}' is not a Host Game setting and was ignored.", key));
        }
    }

    const std::string base_type = setup.m_base["game_type"].value_or<std::string>("DM");
    for (toml::table& level : setup.m_levels) {
        toml::table& rules = child_table(level, "rules");
        if (!rules.contains("game_type")) {
            rules.insert_or_assign("game_type", base_type);
        }
    }

    // As a dedicated server deals them out: the bots take the profiles in turn, and the profiles beyond server_bots
    // stay in the pool.
    const int num_profiles = static_cast<int>(profile_tables.size());
    const int num_bots = std::clamp(server_bots.value_or(0), 0, k_max_server_bots);
    const auto profile_bot = [&](int profile, std::vector<std::string>& profile_warnings) {
        ListenServerBot bot = bot_from_profile(profile_tables[profile], std::format("{}", profile), profile_warnings);
        bot.team = profile < static_cast<int>(teams.size()) ? teams[profile] : -1;
        return bot;
    };
    std::vector<std::string> repeated_warnings;
    for (int i = 0; i < num_bots; ++i) {
        setup.m_bots.push_back(num_profiles > 0
            ? profile_bot(i % num_profiles, i < num_profiles ? warnings : repeated_warnings)
            : ListenServerBot{});
    }
    // The pool leaves room for a full roster, so a setup saved after adding bots still loads whole.
    const int max_pool = static_cast<int>(max_setup_bot_profiles) - k_max_server_bots;
    const int num_kept = std::min(num_profiles, num_bots + max_pool);
    if (num_kept < num_profiles) {
        warnings.push_back(std::format(
            "Only {} bot_profiles entries beyond server_bots were kept; {} more were left out.", max_pool,
            num_profiles - num_kept));
    }
    for (int i = num_bots; i < num_kept; ++i) {
        setup.m_pool_bots.push_back(profile_bot(i, warnings));
    }

    setup.sync_base_game_type();
    return setup;
}

ListenServerStartResult start_listen_server(const ListenServerSetup& setup)
{
    if (setup.num_levels() == 0) {
        return ListenServerStartResult::NoLevels;
    }
    // Ends whatever game state is left; its multi_stop clears any previous listen config.
    rf::game_shutdown();
    // A level or rules override from the previous session's commands or votes must not apply to this one.
    set_manually_loaded_level(false);

    std::vector<toml::table> profiles;
    for (const ListenServerBot& bot : setup.bot_profiles()) {
        profiles.push_back(bot_profile_table(bot));
    }
    toml::table root = setup.to_config_table({});
    if (toml::table* base = root.get_as<toml::table>("base"); base && !base->contains("geo_limit")) {
        base->insert("geo_limit", default_geo_limit);
    }
    root.insert("server_name", setup.server_name());
    install_listen_server_config(root, profiles);
    const AlpineServerConfig& cfg = g_alpine_server_config;
    if (cfg.levels.empty()) {
        clear_listen_server_config();
        return ListenServerStartResult::NoLevels;
    }

    auto& netgame = rf::netgame;
    start_rotation_from_cfg();
    // multi_load_next_level advances it to the first entry.
    netgame.current_level_index = -1;
    netgame.name = cfg.server_name.c_str();
    netgame.password = cfg.password.c_str();
    g_alpine_server_config_active_rules = cfg.levels.front().rule_overrides;
    netgame.type = g_alpine_server_config_active_rules.game_type;
    set_upcoming_game_type(netgame.type);

    netgame.flags = 0;
    apply_alpine_dedicated_server_rules(netgame, g_alpine_server_config_active_rules);
    const bool lan_only = g_alpine_game_config.create_game_lan_only;
    multi_start_listen_server(cfg.max_players, netgame.flags | (lan_only ? 0 : rf::NG_FLAG_NOT_LAN_ONLY));

    if (lan_only) {
        return ListenServerStartResult::Started;
    }
    if (!rf::tracker_init()) {
        netgame.flags &= ~rf::NG_FLAG_NOT_LAN_ONLY;
        return ListenServerStartResult::StartedLanOnly;
    }
    rf::tracker_do_broadcast_server();
    return ListenServerStartResult::Started;
}

ListenServerSetup listen_server_default_setup()
{
    ListenServerSetup setup;
    setup.set_server_name(g_alpine_game_config.create_game_server_name);
    setup.set_password(g_alpine_game_config.create_game_password);
    return setup;
}

std::vector<rf::NetGameType> listen_server_game_types_for_level(std::string_view filename)
{
    std::vector<rf::NetGameType> types;
    for (int type = 0; type < rf::NG_TYPE_UNK; ++type) {
        if (multi_level_name_matches_game_type(filename, static_cast<rf::NetGameType>(type))) {
            types.push_back(static_cast<rf::NetGameType>(type));
        }
    }
    return types;
}

std::vector<std::string> listen_server_levels()
{
    return multi_installed_levels([](const char* name) { return !listen_server_game_types_for_level(name).empty(); });
}

int listen_server_stock_level_rank(std::string_view filename)
{
    static constexpr auto stock_levels = std::to_array<std::string_view>({
        "dm01", "dm02", "dm03", "dm04", "dm05", "dm06", "dm07", "dm08", "dm09", "dm10",
        "dm11", "dm12", "dm13", "dm14", "dm15", "dm16", "dm17", "dm18", "dm19", "dm20",
        "ctf01", "ctf02", "ctf03", "ctf04", "ctf05", "ctf06", "ctf07",
    });
    const std::string name = level_filename_without_ext(filename);
    return static_cast<int>(std::ranges::find_if(stock_levels, [&](std::string_view level) {
        return string_iequals(level, name);
    }) - stock_levels.begin());
}

bool listen_server_setup_name_char(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_';
}

bool listen_server_setup_name_valid(std::string_view name)
{
    if (name.empty() || name.size() > 64 || name.front() == ' ' || name.back() == ' '
        || is_reserved_device_name(name)) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), listen_server_setup_name_char);
}

std::vector<std::string> listen_server_saved_setups()
{
    std::vector<std::string> names;
    const fs::path dir = servers_dir(false);
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) {
        return names;
    }
    for (fs::directory_iterator it{dir, ec}, end; !ec && it != end; it.increment(ec)) {
        const std::optional<std::string> stem = ascii_path_text(it->path().stem());
        const std::optional<std::string> ext = ascii_path_text(it->path().extension());
        std::error_code type_ec;
        if (stem && ext && it->is_regular_file(type_ec) && string_iequals(*ext, ".toml")
            && listen_server_setup_name_valid(*stem)) {
            names.push_back(*stem);
        }
    }
    std::sort(names.begin(), names.end(), string_iless);
    return names;
}

bool listen_server_save_setup(const ListenServerSetup& setup, std::string_view name)
{
    return listen_server_setup_name_valid(name) && save_setup(setup, name);
}

std::optional<ListenServerSetup> listen_server_load_setup(std::string_view name, std::vector<std::string>& warnings)
{
    if (!listen_server_setup_name_valid(name)) {
        return std::nullopt;
    }
    return load_setup(name, warnings);
}

bool listen_server_take_return_to_create_game()
{
    return std::exchange(g_return_to_create_game, false);
}

void listen_server_on_multi_start()
{
    g_return_to_create_game = false;
}

void listen_server_on_multi_stop()
{
    if (!listen_server_config_active()) {
        return;
    }
    clear_listen_server_config();
    g_return_to_create_game = true;
}
