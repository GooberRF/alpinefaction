#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <toml++/toml.hpp>
#include <xlog/xlog.h>
#include <common/alpine_powerup_items.h>
#include <common/utils/string-utils.h>
#include <common/vehicle_meshes.h>
#include "reference_data.h"
#include "../misc/misc.h"
#include "../misc/vpackfile.h"
#include "../multi/gametype.h"
#include "../multi/multi.h"
#include "../multi/powerups.h"
#include "../multi/vehicles/vehicle.h"
#include "../rf/entity.h"
#include "../rf/file/file.h"
#include "../rf/item.h"
#include "../rf/multi.h"
#include "../rf/weapon.h"

namespace
{

constexpr const char* reference_filename = "af_reference.toml";

// One af_reference.toml record. `key` is the class name, or a gametype's short code.
struct Record
{
    std::string key;
    std::string name;
    std::string category;
    std::string description;
    std::string amount;
    std::string amount_unit;
    int amount_value = -1;
    float preview_zoom = 1.0f;
    int seats = 0;
    bool hidden = false;
};

std::array<std::vector<ReferenceEntry>, reference_area_count> g_areas;
// Display names of multiplayer characters, by pc_multi.tbl $Name.
std::vector<Record> g_mp_characters;

std::optional<toml::table> load_reference_file()
{
    const std::optional<std::string> content = read_game_file(reference_filename);
    if (!content) {
        if (rf::File{}.find(reference_filename)) {
            xlog::error("[Reference] Could not read {}", reference_filename);
        }
        else {
            xlog::warn("[Reference] {} not found; entries show their table stats only", reference_filename);
        }
        return std::nullopt;
    }
    try {
        return toml::parse(*content, std::string{reference_filename});
    }
    catch (const toml::parse_error& err) {
        xlog::error("[Reference] Failed to parse {}: {}", reference_filename, err.description());
        return std::nullopt;
    }
}

std::vector<Record> read_records(const toml::table& root, const char* array_name, const char* key_name)
{
    std::vector<Record> records;
    const toml::array* array = root[array_name].as_array();
    if (!array) {
        return records;
    }
    for (const toml::node& node : *array) {
        const toml::table* t = node.as_table();
        if (!t) {
            continue;
        }
        Record r;
        r.key = (*t)[key_name].value_or(std::string{});
        if (r.key.empty()) {
            xlog::warn("[Reference] A [[{}]] record in {} has no {}", array_name, reference_filename, key_name);
            continue;
        }
        r.name = (*t)["name"].value_or(std::string{});
        r.category = (*t)["category"].value_or(std::string{});
        r.description = (*t)["description"].value_or(std::string{});
        r.amount = (*t)["amount"].value_or(std::string{});
        r.amount_unit = (*t)["amount_unit"].value_or(std::string{});
        r.amount_value = static_cast<int>((*t)["amount_value"].value_or(int64_t{-1}));
        r.preview_zoom = std::clamp((*t)["preview_zoom"].value_or(1.0f), 0.1f, 10.0f);
        r.seats = static_cast<int>((*t)["seats"].value_or(int64_t{0}));
        r.hidden = (*t)["hidden"].value_or(false);
        records.push_back(std::move(r));
    }
    return records;
}

// The records' entries in the file's order, then the table's other entries in table order. A record
// can name anything `listed` accepts; only what `unlisted` accepts appears without one. A hidden record
// takes its entry out.
template<typename NameOf, typename Listed, typename Unlisted>
std::vector<std::pair<int, const Record*>> order_entries(const std::vector<Record>& records, int count,
                                                         NameOf name_of, Listed listed, Unlisted unlisted)
{
    std::vector<std::pair<int, const Record*>> out;
    std::vector<bool> taken(static_cast<std::size_t>(std::max(count, 0)), false);
    for (const Record& r : records) {
        bool found = false;
        for (int i = 0; i < count; ++i) {
            if (!taken[i] && listed(i) && string_iequals(name_of(i), r.key)) {
                taken[i] = true;
                found = true;
                if (!r.hidden) {
                    out.emplace_back(i, &r);
                }
                break;
            }
        }
        if (!found) {
            xlog::debug("[Reference] {} names '{}', which the loaded tables do not have", reference_filename, r.key);
        }
    }
    for (int i = 0; i < count; ++i) {
        if (!taken[i] && unlisted(i)) {
            out.emplace_back(i, nullptr);
        }
    }
    return out;
}

std::string format_number(float value, int max_decimals = 1)
{
    if (std::fabs(value - std::round(value)) < 0.005f) {
        return std::to_string(std::lround(value));
    }
    std::string text = std::format("{:.{}f}", value, max_decimals);
    while (text.back() == '0') {
        text.pop_back();
    }
    if (text.back() == '.') {
        text.pop_back();
    }
    return text;
}

void add_stat(std::vector<ReferenceStat>& stats, std::string label, std::string value, std::string mp_value = {})
{
    if (mp_value == value) {
        mp_value.clear();
    }
    stats.push_back({std::move(label), std::move(value), std::move(mp_value)});
}

std::string with_unit(const std::string& value, std::string_view unit)
{
    return unit.empty() ? value : value + " " + std::string{unit};
}

std::string display_or(const Record* record, std::string_view fallback)
{
    return record && !record->name.empty() ? record->name : std::string{fallback};
}

std::string description_of(const Record* record)
{
    return record ? record->description : std::string{};
}

float preview_zoom_of(const Record* record)
{
    return record ? record->preview_zoom : 1.0f;
}

std::string category_or(const Record* record, std::string fallback)
{
    return record && !record->category.empty() ? record->category : std::move(fallback);
}

// ---------------------------------------------------------------------------
// Weapons
// ---------------------------------------------------------------------------

bool is_player_weapon(int type)
{
    return (rf::weapon_types[type].flags & rf::WTF_PLAYER_WEP) != 0;
}

// The pickup's mesh is the one players see lying around; the held mesh is the fallback.
std::string weapon_preview_mesh(int type)
{
    for (int i = 0; i < rf::num_item_types; ++i) {
        const rf::ItemInfo& item = rf::item_info[i];
        if (item.gives_weapon_id == type && !item.v3d_filename.empty()) {
            return item.v3d_filename.c_str();
        }
    }
    return rf::weapon_types[type].third_person_vmesh_filename.c_str();
}

// For a weapon with no record. The fire-mode flags say too little about how a weapon is used, so only the
// unambiguous ones name it.
std::string weapon_kind(const rf::WeaponInfo& w)
{
    if (w.flags & rf::WTF_MELEE) {
        return "Melee";
    }
    if (w.flags & rf::WTF_REMOTE_CHARGE) {
        return "Explosive, remote detonated";
    }
    if (w.damage_radius_single > 0.0f || w.damage_radius_multi > 0.0f) {
        return "Explosive";
    }
    if (w.flags & rf::WTF_SEMI_AUTOMATIC) {
        return "Semi-automatic";
    }
    return {};
}

std::string weapon_features(int type, const rf::WeaponInfo& w)
{
    std::vector<const char*> features;
    if (w.flags & rf::WTF_ALT_FIRE) {
        features.push_back("alternate fire");
    }
    if (w.flags & rf::WTF_ALT_ZOOM) {
        features.push_back("zoom");
    }
    if (rf::weapon_has_scanner(type)) {
        features.push_back("scanner");
    }
    if (w.flags & rf::WTF_HOMING) {
        features.push_back("homing");
    }
    if (w.flags & rf::WTF_PIERCING) {
        features.push_back("piercing");
    }
    if (w.flags & rf::WTF_STICKY) {
        features.push_back("sticky");
    }
    if (w.flags & rf::WTF_SILENT) {
        features.push_back("silenced");
    }
    if (w.flags & rf::WTF_UNDERWATER) {
        features.push_back("fires underwater");
    }
    std::string text;
    for (const char* f : features) {
        text += text.empty() ? std::string{f} : std::string{", "} + f;
    }
    return string_capitalize(text);
}

std::string damage_text(float damage, int projectiles)
{
    std::string text = format_number(damage);
    if (projectiles > 1) {
        text += " x " + std::to_string(projectiles);
    }
    return text;
}

// The fire wait a player gets: Alpine replaces the stock semi-automatics' 500 ms (object/weapon.cpp).
int player_fire_wait_ms(int type, bool alt)
{
    const int ms = rf::weapon_get_fire_wait_ms(type, alt);
    constexpr int stock_semi_auto_wait_ms = 500;
    return (rf::weapon_is_semi_automatic(type) && ms == stock_semi_auto_wait_ms) ? default_semi_auto_fire_wait_ms : ms;
}

ReferenceEntry build_weapon(int type, const Record* record)
{
    const rf::WeaponInfo& w = rf::weapon_types[type];
    ReferenceEntry e;
    e.name = display_or(record, w.display_name.empty() ? w.name.c_str() : w.display_name.c_str());
    e.subtitle = category_or(record, weapon_kind(w));
    e.description = description_of(record);
    e.mesh = weapon_preview_mesh(type);
    e.preview_zoom = preview_zoom_of(record);

    const bool melee = (w.flags & rf::WTF_MELEE) != 0;
    const bool alt_fire = (w.flags & rf::WTF_ALT_FIRE) != 0;
    add_stat(e.stats, "Damage", damage_text(w.damage, w.num_projectiles),
             damage_text(w.damage_multi, w.num_projectiles));
    if (alt_fire && (w.alt_damage != w.damage || w.alt_damage_multi != w.damage_multi)) {
        add_stat(e.stats, "Alt damage", damage_text(w.alt_damage, w.num_projectiles),
                 damage_text(w.alt_damage_multi, w.num_projectiles));
    }
    add_stat(e.stats, "Fire interval", std::to_string(player_fire_wait_ms(type, false)) + " ms");
    if (alt_fire && w.alt_fire_wait != w.fire_wait) {
        add_stat(e.stats, "Alt fire interval", std::to_string(player_fire_wait_ms(type, true)) + " ms");
    }
    if (rf::weapon_uses_clip(type) && w.clip_size_single > 0) {
        add_stat(e.stats, "Magazine", std::to_string(w.clip_size_single), std::to_string(w.clip_size_multi));
        if (w.clip_reload_time_secs > 0.0f) {
            add_stat(e.stats, "Reload", format_number(w.clip_reload_time_secs, 2) + " s");
        }
    }
    if (w.max_ammo_single > 0 || w.max_ammo_multi > 0) {
        add_stat(e.stats, "Max ammo", std::to_string(w.max_ammo_single), std::to_string(w.max_ammo_multi));
    }
    if (!melee && w.max_speed_single > 0.0f) {
        add_stat(e.stats, "Projectile speed", format_number(w.max_speed_single) + " m/s",
                 format_number(w.max_speed_multi) + " m/s");
        const bool ranged = !(w.flags & (rf::WTF_GRAVITY | rf::WTF_REMOTE_CHARGE));
        if (ranged && w.lifetime_seconds_single > 0.0f) {
            add_stat(e.stats, "Range", format_number(w.lifetime_seconds_single * w.max_speed_single) + " m",
                     format_number(w.lifetime_seconds_multi * w.max_speed_multi) + " m");
        }
    }
    if (w.spread_degrees_single > 0.0f || w.spread_degrees_multi > 0.0f) {
        add_stat(e.stats, "Spread", format_number(w.spread_degrees_single, 2) + " deg",
                 format_number(w.spread_degrees_multi, 2) + " deg");
    }
    if (w.damage_radius_single > 0.0f || w.damage_radius_multi > 0.0f) {
        add_stat(e.stats, "Blast radius", format_number(w.damage_radius_single) + " m",
                 format_number(w.damage_radius_multi) + " m");
    }
    if (const std::string features = weapon_features(type, w); !features.empty()) {
        add_stat(e.stats, "Features", features);
    }
    return e;
}

std::vector<ReferenceEntry> build_weapons(const std::vector<Record>& records)
{
    const auto entries = order_entries(
        records, rf::num_weapon_types, [](int i) { return rf::weapon_types[i].name.c_str(); }, is_player_weapon,
        is_player_weapon);
    std::vector<ReferenceEntry> out;
    for (const auto& [type, record] : entries) {
        out.push_back(build_weapon(type, record));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Vehicles
// ---------------------------------------------------------------------------

// What a Vehicle Factory can spawn: the synced vehicle and turret classes it supports.
bool is_vehicle_type(int type)
{
    return vehicle_is_vehicle_entity_type(type)
        && !vehicle_factory_class_is_unsupported(rf::entity_types[type].name.c_str());
}

// Multiplayer spawns the Alpine hull where there is one.
std::string vehicle_preview_mesh(const rf::EntityInfo& info)
{
    const char* alpine_hull = alpine_vehicle_class_mesh(info.name.c_str());
    return alpine_hull ? alpine_hull : info.vmesh_filename.c_str();
}

ReferenceEntry build_vehicle(int type, const Record* record)
{
    const rf::EntityInfo& info = rf::entity_types[type];
    ReferenceEntry e;
    e.name = display_or(record, info.name.c_str());
    const std::string kind = string_capitalize(vehicle_class_display_name(vehicle_class_from_type(type)));
    e.subtitle = category_or(record, string_iequals(kind, e.name) ? std::string{} : kind);
    e.description = description_of(record);
    e.mesh = vehicle_preview_mesh(info);
    e.preview_zoom = preview_zoom_of(record);
    e.vehicle_type = type;

    const std::optional<float> mp_life = vehicle_mp_max_life(info.name.c_str());
    add_stat(e.stats, "Health", format_number(info.max_life), format_number(mp_life.value_or(info.max_life)));
    if (info.max_armor > 0.0f) {
        add_stat(e.stats, "Armor", format_number(info.max_armor));
    }
    if (info.max_vel > 0.0f) {
        add_stat(e.stats, "Top speed", format_number(info.max_vel) + " m/s");
    }
    // No weapons line: entity.tbl's parser marks $Allowed Weapons by list position, not weapon type.
    if (record && record->seats > 0) {
        add_stat(e.stats, "Seats", std::to_string(record->seats));
    }
    return e;
}

std::vector<ReferenceEntry> build_vehicles(const std::vector<Record>& records)
{
    const auto entries = order_entries(
        records, rf::num_entity_types, [](int i) { return rf::entity_types[i].name.c_str(); }, is_vehicle_type,
        is_vehicle_type);
    std::vector<ReferenceEntry> out;
    for (const auto& [type, record] : entries) {
        out.push_back(build_vehicle(type, record));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Gametypes
// ---------------------------------------------------------------------------

const char* gametype_origin(rf::NetGameType type)
{
    switch (type) {
        case rf::NG_TYPE_DM:
        case rf::NG_TYPE_CTF:
        case rf::NG_TYPE_TEAMDM:
            return "Red Faction";
        case rf::NG_TYPE_KOTH:
        case rf::NG_TYPE_DC:
        case rf::NG_TYPE_REV:
        case rf::NG_TYPE_RUN:
        case rf::NG_TYPE_ESC:
            return "Alpine Faction 1.2";
        case rf::NG_TYPE_BAG:
        case rf::NG_TYPE_TBAG:
        case rf::NG_TYPE_PIT:
        case rf::NG_TYPE_WO:
        case rf::NG_TYPE_GG:
        case rf::NG_TYPE_SAL:
            return "Alpine Faction 1.4";
        case rf::NG_TYPE_SCAV:
        case rf::NG_TYPE_PAY:
            return "Alpine Faction 1.5";
        case rf::NG_TYPE_UNK:
            break;
    }
    return nullptr;
}

// The short help text reads "Name: what to do"; the name already heads the entry.
std::string gametype_help(rf::NetGameType type)
{
    const char* help = multi_gametype_help_text(type);
    if (!help) {
        return {};
    }
    const std::string_view text{help};
    const std::size_t colon = text.find(": ");
    return std::string{colon == std::string_view::npos ? text : text.substr(colon + 2)};
}

ReferenceEntry build_gametype(int index, const Record* record)
{
    const auto type = static_cast<rf::NetGameType>(index);
    const bool team = multi_game_type_is_team_type(type);
    ReferenceEntry e;
    e.name = display_or(record, multi_game_type_name(type));
    e.subtitle = category_or(record, team ? "Team game" : "Free for all");
    e.description = record && !record->description.empty() ? record->description : gametype_help(type);
    add_stat(e.stats, "Short name", std::string{multi_game_type_name_short(type)});
    add_stat(e.stats, "Teams", team ? "Red vs Blue" : "None");
    add_stat(e.stats, "Rounds", gt_type_uses_rounds(type) ? "Yes" : "No");
    if (const char* origin = gametype_origin(type)) {
        add_stat(e.stats, "Introduced in", origin);
    }
    return e;
}

std::vector<ReferenceEntry> build_gametypes(const std::vector<Record>& records)
{
    const auto any = [](int) { return true; };
    const auto entries = order_entries(
        records, static_cast<int>(rf::NG_TYPE_UNK),
        [](int i) { return multi_game_type_name_short(static_cast<rf::NetGameType>(i)); }, any, any);
    std::vector<ReferenceEntry> out;
    for (const auto& [type, record] : entries) {
        out.push_back(build_gametype(type, record));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Powerups
// ---------------------------------------------------------------------------

const AlpinePowerupItemClass* alpine_powerup_class(int item_type)
{
    for (std::size_t i = 0; i < std::size(alpine_powerup_item_classes); ++i) {
        if (powerup_item_type(static_cast<AlpinePowerup>(i)) == item_type) {
            return &alpine_powerup_item_classes[i];
        }
    }
    return nullptr;
}

const char* powerup_category(const AlpinePowerupItemClass* alpine, const Record* record, std::string_view cls)
{
    if (alpine) {
        switch (alpine->effect) {
            case AlpinePowerupEffect::timed:
                return "Timed powerup";
            case AlpinePowerupEffect::health:
                return "Health";
            case AlpinePowerupEffect::armor:
                return "Armor";
        }
    }
    if (record && record->amount_unit == "s") {
        return "Timed powerup";
    }
    const bool health = string_icontains(cls, "health") || string_icontains(cls, "med") || string_icontains(cls, "aid");
    const bool armor = string_icontains(cls, "armor") || string_icontains(cls, "repair");
    return health ? "Health" : armor ? "Armor" : "Equipment";
}

ReferenceEntry build_powerup(int type, const Record* record)
{
    const rf::ItemInfo& info = rf::item_info[type];
    const AlpinePowerupItemClass* alpine = alpine_powerup_class(type);
    ReferenceEntry e;
    e.name = display_or(record, alpine ? alpine->hud_msg_name : info.cls_name.c_str());
    e.subtitle = category_or(record, powerup_category(alpine, record, info.cls_name.c_str()));
    e.description = description_of(record);
    e.mesh = info.v3d_filename.c_str();
    e.preview_zoom = preview_zoom_of(record);

    std::string label = record ? record->amount : std::string{};
    std::string unit = record ? record->amount_unit : std::string{};
    if (label.empty() && alpine) {
        label = alpine->effect == AlpinePowerupEffect::timed ? "Duration" : "Amount";
        unit = alpine->effect == AlpinePowerupEffect::timed ? "s"
            : alpine->effect == AlpinePowerupEffect::health  ? "health"
                                                             : "armor";
    }
    // Timed powerups do nothing in single player, so only their multiplayer values are shown.
    e.multiplayer_only = alpine ? alpine->effect == AlpinePowerupEffect::timed
                                : string_iequals(info.cls_name.c_str(), "Multi Damage Amplifier")
                                      || string_iequals(info.cls_name.c_str(), "Multi Invulnerability");
    if (!label.empty() && record && record->amount_value >= 0) {
        add_stat(e.stats, label, with_unit(std::to_string(record->amount_value), unit));
    }
    else if (!label.empty()) {
        const std::string multi = with_unit(std::to_string(info.count_multi), unit);
        if (e.multiplayer_only) {
            add_stat(e.stats, label, multi);
        }
        else {
            add_stat(e.stats, label, with_unit(std::to_string(info.count_single), unit), multi);
        }
    }
    // Super items and shards can raise their value past the normal maximum.
    if (string_iequals(e.subtitle, "Health") || string_iequals(e.subtitle, "Armor")) {
        add_stat(e.stats, "Is Super?", item_type_is_super(type) || item_type_is_alpine_shard(type) ? "Yes" : "No");
    }
    // Single player never respawns items.
    if (info.respawn_time_millis > 0) {
        const std::string respawn = format_number(info.respawn_time_millis / 1000.0f) + " s";
        if (e.multiplayer_only) {
            add_stat(e.stats, "Respawn", respawn);
        }
        else {
            add_stat(e.stats, "Respawn", "None", respawn);
        }
    }
    return e;
}

std::vector<ReferenceEntry> build_powerups(const std::vector<Record>& records)
{
    // Only the records pick stock items; every Alpine powerup appears even without one.
    const auto entries = order_entries(
        records, rf::num_item_types, [](int i) { return rf::item_info[i].cls_name.c_str(); },
        [](int) { return true; }, [](int i) { return alpine_powerup_class(i) != nullptr; });
    std::vector<ReferenceEntry> out;
    for (const auto& [type, record] : entries) {
        out.push_back(build_powerup(type, record));
    }
    return out;
}

} // namespace

void reference_data_load()
{
    const std::optional<toml::table> root = load_reference_file();
    const toml::table empty;
    const toml::table& file = root ? *root : empty;
    // The weapon records describe the stock weapons, which a TC mod's own weapons.tbl can redefine, so its weapons
    // show as its table has them. A TC that ships its own af_reference.toml is taken to describe them as they now are.
    const bool tc_weapons_undescribed =
        file_loaded_from_tc_mod("weapons.tbl") && !file_loaded_from_tc_mod(reference_filename);
    g_areas[static_cast<int>(ReferenceArea::weapons)] =
        build_weapons(tc_weapons_undescribed ? std::vector<Record>{} : read_records(file, "weapon", "class"));
    g_areas[static_cast<int>(ReferenceArea::vehicles)] = build_vehicles(read_records(file, "vehicle", "class"));
    g_areas[static_cast<int>(ReferenceArea::gametypes)] = build_gametypes(read_records(file, "gametype", "code"));
    g_areas[static_cast<int>(ReferenceArea::powerups)] = build_powerups(read_records(file, "powerup", "class"));
    g_mp_characters = read_records(file, "mp_character", "character");
}

int mp_character_count()
{
    return std::clamp(rf::num_multi_characters, 0, rf::MAX_MP_CHARACTERS);
}

const char* mp_character_caption(int index)
{
    if (index < 0 || index >= mp_character_count()) {
        return nullptr;
    }
    const rf::MpCharacterInfo& info = rf::mp_characters[index];
    const std::string_view key{info.name, strnlen(info.name, sizeof(info.name))};
    const auto it = std::ranges::find_if(g_mp_characters, [&](const Record& r) { return string_iequals(r.key, key); });
    if (it != g_mp_characters.end() && !it->name.empty()) {
        return it->name.c_str();
    }
    if (info.screen_name && info.screen_name[0] != '\0') {
        return info.screen_name;
    }
    return info.name[0] != '\0' ? info.name : nullptr;
}

const std::vector<ReferenceEntry>& reference_area_entries(ReferenceArea area)
{
    return g_areas[static_cast<int>(area)];
}

const char* reference_area_label(ReferenceArea area)
{
    switch (area) {
        case ReferenceArea::weapons:
            return "WEAPONS";
        case ReferenceArea::vehicles:
            return "VEHICLES";
        case ReferenceArea::gametypes:
            return "GAMETYPES";
        case ReferenceArea::powerups:
            return "POWERUPS";
    }
    return "";
}
