#include "tbl_overrides.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <patch_common/FunHook.h>

#include "../multi/server_internal.h"
#include "../os/console.h"
#include "../rf/entity.h"
#include "../rf/multi.h"
#include "../rf/os/console.h"
#include "../rf/os/os.h"
#include "../rf/weapon.h"
#include "tbl_overrides_baseline.h"

static_assert(sizeof(rf::EntityCollisionSphereOverride) == 0x30);
static_assert(offsetof(rf::EntityCollisionSphereOverride, damage_factor_multi) == 0xC);
static_assert(offsetof(rf::EntityInfo, csphere_overrides) == 0xB68);

namespace afstats {

namespace {

struct CapturedWeapon
{
    std::string name;
    float collision_radius = 0.0f;
    int max_ammo = 0;
    int clip_size = 0;
    int num_projectiles = 0;
    float damage = 0.0f;
    float alt_damage = 0.0f;
    float spread = 0.0f;
    float alt_spread = 0.0f;
    float damage_radius = 0.0f;
    float crater_radius = 0.0f;
    float bbox_factor = 0.0f;
    bool burst = false;
    bool burst_alt = false;
    int burst_count = 0;
    float burst_delay = 0.0f;
    bool piercing = false;
    float piercing_power = 0.0f;
    float ricochet_cos = 0.0f;
};

struct CapturedSphere
{
    std::string name;
    float damage_factor_multi = 0.0f;
};

struct CapturedEntity
{
    std::string name;
    std::vector<CapturedSphere> spheres;
};

std::vector<CapturedWeapon> g_weapons;
std::vector<CapturedEntity> g_entities;

// A TC mod without require_client_mod is a server-side mod.
// Only a real TC mod is skipped.
bool tc_mod_active()
{
    return rf::mod_param.found() && !server_is_modded();
}

void capture_weapons()
{
    g_weapons.clear();
    const int count = std::clamp(rf::num_weapon_types, 0, 64);
    g_weapons.reserve(count);
    for (int i = 0; i < count; ++i) {
        const rf::WeaponInfo& info = rf::weapon_types[i];
        CapturedWeapon w{info.name};
        w.collision_radius = info.collision_radius;
        w.max_ammo = info.max_ammo_multi;
        w.clip_size = info.clip_size_multi;
        w.num_projectiles = info.num_projectiles;
        w.damage = info.damage_multi;
        w.alt_damage = info.alt_damage_multi;
        w.spread = info.spread_degrees_multi;
        w.alt_spread = info.alt_spread_degrees_multi;
        w.damage_radius = info.damage_radius_multi;
        w.crater_radius = info.crater_radius;
        w.bbox_factor = info.multi_bbox_size_factor;
        w.burst = (info.flags & rf::WTF_BURST_MODE) != 0;
        w.burst_alt = (info.flags & rf::WTF_BURST_MODE_ALT_FIRE) != 0;
        w.burst_count = info.burst_count;
        w.burst_delay = info.burst_delay_seconds;
        w.piercing = (info.flags & rf::WTF_PIERCING) != 0;
        w.piercing_power = info.pierce_power;
        w.ricochet_cos = info.ricochet_angle_cos;
        g_weapons.push_back(std::move(w));
    }
}

void capture_entities()
{
    g_entities.clear();
    const int count = std::clamp(rf::num_entity_types, 0, rf::MAX_ENTITY_TYPES);
    g_entities.reserve(count);
    for (int i = 0; i < count; ++i) {
        const rf::EntityInfo& info = rf::entity_types[i];
        CapturedEntity e{info.name};
        for (const rf::EntityCollisionSphereOverride& sphere : info.csphere_overrides) {
            CapturedSphere s;
            const auto* end = static_cast<const char*>(std::memchr(sphere.name, '\0', sizeof(sphere.name)));
            s.name.assign(sphere.name, end ? static_cast<size_t>(end - sphere.name) : sizeof(sphere.name));
            s.damage_factor_multi = sphere.damage_factor_multi;
            e.spheres.push_back(std::move(s));
        }
        g_entities.push_back(std::move(e));
    }
}

FunHook<void()> weapons_tbl_parse_hook{
    0x004C67A0,
    []() {
        weapons_tbl_parse_hook.call_target();
        if (rf::is_dedicated_server) {
            capture_weapons();
        }
    },
};

FunHook<void()> entity_tbl_parse_hook{
    0x0041B830,
    []() {
        entity_tbl_parse_hook.call_target();
        if (rf::is_dedicated_server) {
            capture_entities();
        }
    },
};

// -------------------------------------------------------------------------
// Evaluation
// -------------------------------------------------------------------------

bool float_differs(float a, float b)
{
    return std::bit_cast<uint32_t>(a) != std::bit_cast<uint32_t>(b);
}

// Round-trips through the shortest decimal that reproduces the float, so a 0.1f
// value lands on the wire as 0.1 rather than the double widening of its bits.
double json_float(float value)
{
    return std::strtod(std::format("{}", value).c_str(), nullptr);
}

double ricochet_degrees(float cos_value)
{
    const double clamped = std::clamp(static_cast<double>(cos_value), -1.0, 1.0);
    const double degrees = std::acos(clamped) * (180.0 / std::numbers::pi);
    return std::round(degrees * 100.0) / 100.0;
}

struct Deviations
{
    nlohmann::json fields = nlohmann::json::object();
    std::string owner;
    bool verbose = false;

    void emit(const char* key, nlohmann::json value, nlohmann::json stock, const std::string& note)
    {
        nlohmann::json leaf;
        leaf["value"] = std::move(value);
        leaf["stock"] = std::move(stock);
        fields[key] = std::move(leaf);
        if (verbose) {
            rf::console::print("  {}.{}: {}", owner, key, note);
        }
    }

    void add_bool(const char* key, bool live, bool stock)
    {
        if (live == stock) {
            return;
        }
        emit(key, live, stock, std::format("{} (stock {})", live, stock));
    }

    void add_int(const char* key, int live, int stock)
    {
        if (live == stock) {
            return;
        }
        emit(key, live, stock, std::format("{} (stock {})", live, stock));
    }

    void add_int_added(const char* key, int live)
    {
        emit(key, live, nullptr, std::format("{} (no stock value)", live));
    }

    void add_float(const char* key, float live, float stock)
    {
        if (!float_differs(live, stock)) {
            return;
        }
        emit(key, json_float(live), json_float(stock),
             std::format("{} (stock {}) [0x{:08X} vs 0x{:08X}]", live, stock, std::bit_cast<uint32_t>(live),
                         std::bit_cast<uint32_t>(stock)));
    }

    void add_float_added(const char* key, float live)
    {
        emit(key, json_float(live), nullptr, std::format("{} (no stock value)", live));
    }

    void add_degrees(const char* key, double live, std::optional<double> stock)
    {
        if (stock) {
            emit(key, live, *stock, std::format("{} (stock {})", live, *stock));
        }
        else {
            emit(key, live, nullptr, std::format("{} (no stock value)", live));
        }
    }
};

const CapturedWeapon* find_weapon(const char* name)
{
    const auto it = std::find_if(g_weapons.begin(), g_weapons.end(),
                                 [name](const CapturedWeapon& w) { return w.name == name; });
    return it != g_weapons.end() ? &*it : nullptr;
}

const tbl::StockEntity* find_stock_entity(const std::string& name)
{
    for (int i = 0; i < tbl::g_num_stock_entities; ++i) {
        if (name == tbl::g_stock_entities[i].name) {
            return &tbl::g_stock_entities[i];
        }
    }
    return nullptr;
}

const CapturedSphere* find_sphere(const CapturedEntity& entity, const char* name)
{
    const auto it = std::find_if(entity.spheres.begin(), entity.spheres.end(),
                                 [name](const CapturedSphere& s) { return s.name == name; });
    return it != entity.spheres.end() ? &*it : nullptr;
}

const char* sphere_key(const char* tbl_name)
{
    if (std::strcmp(tbl_name, "csphere_0") == 0) {
        return "legs";
    }
    if (std::strcmp(tbl_name, "csphere_1") == 0) {
        return "torso";
    }
    if (std::strcmp(tbl_name, "csphere_2") == 0) {
        return "head";
    }
    return tbl_name;
}

nlohmann::json eval_weapons(bool verbose)
{
    auto out = nlohmann::json::object();
    for (int i = 0; i < tbl::g_num_stock_weapons; ++i) {
        const tbl::StockWeapon& stock = tbl::g_stock_weapons[i];
        const CapturedWeapon* live = find_weapon(stock.name);
        if (!live) {
            continue;
        }
        Deviations d{.owner = stock.name, .verbose = verbose};
        d.add_float("collision_radius", live->collision_radius, stock.collision_radius);
        d.add_int("max_ammo", live->max_ammo, stock.max_ammo);
        d.add_int("clip_size", live->clip_size, stock.clip_size);
        d.add_int("num_projectiles", live->num_projectiles, stock.num_projectiles);
        d.add_float("damage", live->damage, stock.damage);
        d.add_float("alt_damage", live->alt_damage, stock.alt_damage);
        d.add_float("spread", live->spread, stock.spread);
        d.add_float("alt_spread", live->alt_spread, stock.alt_spread);
        d.add_float("damage_radius", live->damage_radius, stock.damage_radius);
        d.add_float("crater_radius", live->crater_radius, stock.crater_radius);
        d.add_float("bbox_factor", live->bbox_factor, stock.bbox_factor);
        d.add_bool("burst", live->burst, stock.burst);
        d.add_bool("burst_alt", live->burst_alt, stock.burst_alt);

        const bool live_burst = live->burst || live->burst_alt;
        const bool stock_burst = stock.burst || stock.burst_alt;
        if (live_burst && stock_burst) {
            d.add_int("burst_count", live->burst_count, stock.burst_count);
            d.add_float("burst_delay", live->burst_delay, stock.burst_delay);
        }
        else if (live_burst) {
            d.add_int_added("burst_count", live->burst_count);
            d.add_float_added("burst_delay", live->burst_delay);
        }

        d.add_bool("piercing", live->piercing, stock.piercing);
        if (live->piercing && stock.piercing) {
            d.add_float("piercing_power", live->piercing_power, stock.piercing_power);
            if (float_differs(live->ricochet_cos, stock.ricochet_cos)) {
                d.add_degrees("ricochet_angle", ricochet_degrees(live->ricochet_cos),
                              json_float(stock.ricochet_deg));
            }
        }
        else if (live->piercing) {
            d.add_float_added("piercing_power", live->piercing_power);
            d.add_degrees("ricochet_angle", ricochet_degrees(live->ricochet_cos), std::nullopt);
        }

        if (!d.fields.empty()) {
            out[stock.name] = std::move(d.fields);
        }
    }
    return out;
}

nlohmann::json eval_entities(bool verbose)
{
    auto out = nlohmann::json::object();
    std::set<int> referenced;
    const int num_characters = std::clamp(rf::num_multi_characters, 0, rf::MAX_MP_CHARACTERS);
    for (int i = 0; i < num_characters; ++i) {
        const int type = rf::mp_characters[i].entity_type;
        if (type >= 0 && static_cast<size_t>(type) < g_entities.size()) {
            referenced.insert(type);
        }
    }
    for (const int type : referenced) {
        const CapturedEntity& live = g_entities[type];
        const tbl::StockEntity* stock = find_stock_entity(live.name);
        if (!stock) {
            continue;
        }
        Deviations d{.owner = live.name, .verbose = verbose};
        for (int i = 0; i < stock->num_spheres; ++i) {
            const tbl::StockSphere& stock_sphere = stock->spheres[i];
            const CapturedSphere* live_sphere = find_sphere(live, stock_sphere.name);
            if (!live_sphere) {
                continue;
            }
            d.add_float(sphere_key(stock_sphere.name), live_sphere->damage_factor_multi,
                        stock_sphere.damage_factor_multi);
        }
        if (!d.fields.empty()) {
            out[live.name] = std::move(d.fields);
        }
    }
    return out;
}

nlohmann::json build_overrides(bool verbose)
{
    if (g_weapons.empty() && g_entities.empty()) {
        return nlohmann::json{};
    }
    auto weapons = eval_weapons(verbose);
    auto entities = eval_entities(verbose);
    if (weapons.empty() && entities.empty()) {
        return nlohmann::json{};
    }
    auto result = nlohmann::json::object();
    if (!weapons.empty()) {
        result["weapons"] = std::move(weapons);
    }
    if (!entities.empty()) {
        result["entities"] = std::move(entities);
    }
    return result;
}

ConsoleCommand2 dbg_tbl_baseline_cmd{
    "dbg_tbl_baseline",
    []() {
        if (!rf::is_dedicated_server) {
            rf::console::print("tbl override capture: skipped, not a dedicated server");
            return;
        }
        rf::console::print("tbl override capture: {} weapon classes, {} entity classes", g_weapons.size(),
                           g_entities.size());
        rf::console::print("stock baseline: {} weapon classes, {} entity classes", tbl::g_num_stock_weapons,
                           tbl::g_num_stock_entities);
        const char* mod = rf::mod_param.get_arg();
        rf::console::print("mod: {}, server_is_modded: {}", rf::mod_param.found() ? (mod ? mod : "(unnamed)") : "none",
                           server_is_modded());
        if (tc_mod_active()) {
            rf::console::print("reporting: skipped, a client-required TC mod is loaded");
            return;
        }
        const auto deviations = build_overrides(true);
        if (deviations.is_null()) {
            rf::console::print("no deviations from stock");
        }
        else {
            rf::console::print("{}", deviations.dump());
        }
    },
    "Report weapons.tbl and entity.tbl deviations from stock as sent to FactionFiles.",
};

} // namespace

const nlohmann::json& get_tbl_overrides()
{
    static const nlohmann::json none;
    if (tc_mod_active()) {
        return none;
    }
    static const nlohmann::json overrides = build_overrides(false);
    return overrides;
}

void tbl_overrides_do_patch()
{
    weapons_tbl_parse_hook.install();
    entity_tbl_parse_hook.install();
    dbg_tbl_baseline_cmd.register_cmd();
}

} // namespace afstats
