#include "rumble.h"
#include "gamepad.h"
#include "../rf/entity.h"
#include "../rf/player/player.h"
#include "../rf/weapon.h"
#include "../misc/alpine_settings.h"
#include <algorithm>

// Weapon fire: continuous (flamethrower, etc.)
static constexpr RumbleEffect k_rumble_weapon_continuous{ 0x2000, 0xC000, 0x6000, 90u };
// Rocket launcher — short sharp bump on launch
static constexpr RumbleEffect k_rumble_rocket_launch{ 0xC000, 0xFFFF, 0xFFFF, 100u };
// Railgun — heavier single-shot kick, slightly stronger than rocket launcher
static constexpr RumbleEffect k_rumble_railgun_shot{ 0xA000, 0xFFFF, 0xFFFF, 110u };
// Shotgun blast — fixed strength
static constexpr RumbleEffect k_rumble_shotgun_blast{ 0xFFFF, 0xFFFF, 0xFFFF, 140u };
// SMG (machine pistol) — light consistent tick per shot
static constexpr RumbleEffect k_rumble_smg_shot{ 0x1800, 0x1000, 0x2000, 65u };
// Sniper / precision rifle — restrained body kick, strong trigger snap
static constexpr RumbleEffect k_rumble_sniper_shot{ 0x9000, 0xFFFF, 0xFFFF, 180u };
// Turret shot: strong body-only pulse (no trigger routing for turrets)
static constexpr RumbleEffect k_rumble_turret_shot{ 0xA800, 0x8C00, 0, 120u };

struct WeaponRumbleProfile {
    const RumbleEffect* preset = nullptr;
    uint16_t lo_mul = 0;
    uint16_t hi_mul = 0;
    uint16_t tr_mul = 0;
};

static constexpr WeaponRumbleProfile k_wp_rocket  { &k_rumble_rocket_launch,  0,      0,      0      };
static constexpr WeaponRumbleProfile k_wp_railgun { &k_rumble_railgun_shot,   0,      0,      0      };
static constexpr WeaponRumbleProfile k_wp_shotgun { &k_rumble_shotgun_blast,  0,      0,      0      };
static constexpr WeaponRumbleProfile k_wp_smg     { &k_rumble_smg_shot,       0,      0,      0      }; // machine pistol
static constexpr WeaponRumbleProfile k_wp_sniper  { &k_rumble_sniper_shot,    0,      0,      0      }; // sniper / precision rifle
static constexpr WeaponRumbleProfile k_wp_ar      { nullptr,                  0x7800, 0x5C00, 0x9400 }; // assault rifle
static constexpr WeaponRumbleProfile k_wp_pistol  { nullptr,                  0x4200, 0x3300, 0x5100 }; // pistol
static constexpr WeaponRumbleProfile k_wp_default { nullptr,                  0x8000, 0x6000, 0xA000 }; // all other weapons

static const WeaponRumbleProfile& get_weapon_profile(int wt)
{
    if (wt == rf::rocket_launcher_weapon_type || wt == rf::shoulder_cannon_weapon_type)
        return k_wp_rocket;
    if (wt == rf::rail_gun_weapon_type)
        return k_wp_railgun;
    if (rf::weapon_is_shotgun(wt))
        return k_wp_shotgun;
    if (wt == rf::machine_pistol_weapon_type
     || wt == rf::machine_pistol_special_weapon_type)
        return k_wp_smg;
    if (wt == rf::assault_rifle_weapon_type)
        return k_wp_ar;
    if (rf::weapon_is_glock(wt))
        return k_wp_pistol;
    if (rf::weapon_has_scanner(wt))
        return k_wp_sniper;
    return k_wp_default;
}

static constexpr RumbleEffect k_rumble_hit_melee_max    { 0x3000, 0x8000, 0, 120u };
static constexpr RumbleEffect k_rumble_hit_explosive_max{ 0xC000, 0xA000, 0, 300u };
static constexpr RumbleEffect k_rumble_hit_fire_max     { 0x3000, 0x9000, 0, 150u };

// Scale a body-only preset by [0,1]. trigger_motor stays 0.
static RumbleEffect rumble_scale(const RumbleEffect& base, float scale)
{
    return {
        static_cast<uint16_t>(scale * base.lo_motor),
        static_cast<uint16_t>(scale * base.hi_motor),
        0u,
        base.duration_ms,
    };
}

static RumbleEffect rumble_weapon_build(int wt, const rf::WeaponInfo& winfo, bool is_alt_fire)
{
    const WeaponRumbleProfile& prof = get_weapon_profile(wt);

    // Fixed preset — strength is constant regardless of fire rate (e.g. shotgun, rocket launcher).
    if (prof.preset)
        return *prof.preset;

    // Fire-rate factor: slower weapons earn a stronger per-shot kick.
    float raw_wait  = is_alt_fire ? winfo.alt_fire_wait : winfo.fire_wait;
    float fire_wait = raw_wait > 0.0f ? raw_wait : 0.5f;
    float factor    = std::clamp(fire_wait / 0.5f, 0.6f, 1.0f);

    bool continuous = (winfo.flags & (rf::WTF_CONTINUOUS_FIRE | rf::WTF_ALT_CONTINUOUS_FIRE)) != 0;
    bool burst      = (winfo.flags & rf::WTF_BURST_MODE) != 0;

    return {
        static_cast<uint16_t>(factor * prof.lo_mul),
        static_cast<uint16_t>(factor * prof.hi_mul),
        static_cast<uint16_t>(factor * prof.tr_mul),
        continuous ? 70u : burst ? 55u : 100u,
    };
}

static bool rumble_weapon_is_skipped(int wt, const rf::WeaponInfo& winfo)
{
    return (winfo.flags & rf::WTF_MELEE)
        || (winfo.flags & rf::WTF_REMOTE_CHARGE)
        || (winfo.flags & rf::WTF_DETONATOR)
        || (wt == rf::grenade_weapon_type);
}

static void rumble_weapon_do_frame()
{
    if (!g_alpine_game_config.gamepad_weapon_rumble_enabled)
        return;
    static int  s_last_fired_ts     = -2; // -2 = uninitialized sentinel; -1 = game's own "invalid"
    static int  s_last_secondary_ts = -2; // tracks ai.next_fire_secondary.value to detect alt-fire
    static bool s_pending_alt_fire  = false; // secondary ts advanced before last_fired_timestamp (delayed projectile)

    auto* lpe = rf::local_player_entity;
    if (!lpe) {
        s_last_fired_ts     = -2;
        s_last_secondary_ts = -2;
        s_pending_alt_fire  = false;
        return;
    }

    // Turret shots are detected via rumble_on_turret_fire() called from the entity-fire hook.
    if (rf::entity_is_on_turret(lpe)) {
        s_last_fired_ts     = -2;
        s_last_secondary_ts = -2;
        s_pending_alt_fire  = false;
        return;
    }

    // While viewing a security camera the player is not firing; keep the sentinel stale
    // so there is no spurious rumble pulse when the camera view is dismissed.
    if (lpe->local_player && lpe->local_player->view_from_handle != -1) {
        s_last_fired_ts     = -2;
        s_last_secondary_ts = -2;
        s_pending_alt_fire  = false;
        return;
    }

    int wt        = lpe->ai.current_primary_weapon;
    int cur_fired = lpe->last_fired_timestamp.value;
    int cur_secondary_ts = lpe->ai.next_fire_secondary.value;

    if (wt < 0 || wt >= rf::num_weapon_types) {
        s_last_fired_ts     = -2;
        s_last_secondary_ts = -2;
        s_pending_alt_fire  = false;
        return;
    }

    const auto& winfo = rf::weapon_types[wt];

    // Flamethrower is a continuous beam: it never updates last_fired_timestamp.
    // Drive its rumble from entity_weapon_is_on instead.
    if (rf::weapon_is_flamethrower(wt)) {
        if (rf::entity_weapon_is_on(lpe->handle, wt))
            gamepad_play_rumble(k_rumble_weapon_continuous);
        s_last_fired_ts     = cur_fired;
        s_last_secondary_ts = cur_secondary_ts;
        s_pending_alt_fire  = false;
        return;
    }

    if (s_last_secondary_ts != -2 && cur_secondary_ts != s_last_secondary_ts
            && !(winfo.flags & rf::WTF_ALT_ZOOM))
        s_pending_alt_fire = true;

    if (!rumble_weapon_is_skipped(wt, winfo)
            && s_last_fired_ts != -2
            && cur_fired != s_last_fired_ts) {
        bool is_alt_fire = s_pending_alt_fire;
        gamepad_play_rumble(rumble_weapon_build(wt, winfo, is_alt_fire), is_alt_fire);
        s_pending_alt_fire = false;
    }

    s_last_fired_ts    = cur_fired;
    s_last_secondary_ts = cur_secondary_ts;
}

void rumble_do_frame()
{
    if (g_alpine_game_config.gamepad_rumble_intensity <= 0.0f)
        return;
    rumble_weapon_do_frame();
}

void rumble_on_player_hit(float damage, int damage_type)
{
    if (g_alpine_game_config.gamepad_rumble_intensity <= 0.0f)
        return;
    if (!g_alpine_game_config.gamepad_environmental_rumble_enabled)
        return;

    RumbleEffect effect;
    if (damage_type == rf::DT_BASH) {
        float scale = std::min(damage / 40.0f, 1.0f);
        effect = rumble_scale(k_rumble_hit_melee_max, scale);
    }
    else if (damage_type == rf::DT_EXPLOSIVE) {
        float scale = std::min(damage / 75.0f, 1.0f);
        effect = rumble_scale(k_rumble_hit_explosive_max, scale);
    }
    else if (damage_type == rf::DT_FIRE) {
        float scale = std::min(damage / 20.0f, 1.0f);
        effect = rumble_scale(k_rumble_hit_fire_max, scale);
    }
    else {
        return;
    }

    gamepad_play_rumble(effect);
}

void rumble_on_turret_fire(rf::Entity* firer)
{
    if (g_alpine_game_config.gamepad_rumble_intensity <= 0.0f || !g_alpine_game_config.gamepad_weapon_rumble_enabled)
        return;
    if (!rf::entity_is_turret(firer))
        return;
    auto* lpe = rf::local_player_entity;
    if (!lpe || !rf::entity_is_on_turret(lpe))
        return;
    gamepad_play_rumble(k_rumble_turret_shot);
}
