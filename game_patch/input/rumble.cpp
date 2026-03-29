#include "rumble.h"
#include "gamepad.h"
#include "../rf/entity.h"
#include "../rf/weapon.h"
#include "../misc/alpine_settings.h"
#include <algorithm>

// Weapon fire — continuous (flamethrower, etc.)
// Trigger Rumbles is also in use if SDL_RumbleGamepadTriggers is detected.
static constexpr RumbleEffect k_rumble_weapon_continuous{ 0x2000, 0xC000, 0x6000, 90u };

// Turret shot — strong body-only pulse (no trigger routing for turrets)
static constexpr RumbleEffect k_rumble_turret_shot{ 0xC000, 0xA000, 0, 120u };

// Environmental / damage — melee
static constexpr RumbleEffect k_rumble_hit_melee_max{ 0x3000, 0x8000, 0, 120u };

// Environmental / damage — explosive
static constexpr RumbleEffect k_rumble_hit_explosive_max{ 0xC000, 0xA000, 0, 300u };

// Environmental / damage — fire
static constexpr RumbleEffect k_rumble_hit_fire_max{ 0x3000, 0x9000, 0, 150u };


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

// Build a weapon-fire RumbleEffect scaled by damage.
// trigger_motor is set proportionally — standard weapon shots participate in trigger routing.
static RumbleEffect rumble_weapon_scaled(const rf::WeaponInfo& winfo)
{
    float dmg   = winfo.damage > 0.0f ? winfo.damage : winfo.alt_damage;
    float scale = std::min(dmg / 200.0f, 1.0f);

    bool continuous = (winfo.flags & (rf::WTF_CONTINUOUS_FIRE | rf::WTF_ALT_CONTINUOUS_FIRE)) != 0;
    bool burst      = (winfo.flags & rf::WTF_BURST_MODE) != 0;

    return {
        static_cast<uint16_t>(scale * 0x8000),
        static_cast<uint16_t>(scale * 0x6000),
        static_cast<uint16_t>(scale * 0xA000), // trigger motor
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
    static int s_last_fired_ts = -2; // -2 = uninitialized sentinel; -1 = game's own "invalid"

    auto* lpe = rf::local_player_entity;
    if (!lpe) {
        s_last_fired_ts = -2;
        return;
    }

    // Turret shots are detected via rumble_on_turret_fire() called from the entity-fire hook.
    // While on a turret the player entity's last_fired_timestamp never advances, so keep the
    // sentinel stale to avoid a spurious pulse on dismount.
    if (rf::entity_is_on_turret(lpe)) {
        s_last_fired_ts = -2;
        return;
    }

    int wt        = lpe->ai.current_primary_weapon;
    int cur_fired = lpe->last_fired_timestamp.value;

    if (wt < 0 || wt >= rf::num_weapon_types) {
        s_last_fired_ts = -2;
        return;
    }

    const auto& winfo = rf::weapon_types[wt];

    // Flamethrower is a continuous beam — it never updates last_fired_timestamp.
    // Drive its rumble from entity_weapon_is_on instead.
    if (rf::weapon_is_flamethrower(wt)) {
        if (rf::entity_weapon_is_on(lpe->handle, wt))
            gamepad_play_rumble(k_rumble_weapon_continuous);
        s_last_fired_ts = cur_fired;
        return;
    }

    if (!rumble_weapon_is_skipped(wt, winfo)
            && s_last_fired_ts != -2
            && cur_fired != s_last_fired_ts) {
        gamepad_play_rumble(rumble_weapon_scaled(winfo));
    }

    s_last_fired_ts = cur_fired;
}

void rumble_do_frame()
{
    if (!g_alpine_game_config.gamepad_rumble_enabled)
        return;
    rumble_weapon_do_frame();
}

void rumble_on_player_hit(float damage, int damage_type)
{
    if (!g_alpine_game_config.gamepad_rumble_enabled)
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
    if (!g_alpine_game_config.gamepad_rumble_enabled || !g_alpine_game_config.gamepad_weapon_rumble_enabled)
        return;
    if (!rf::entity_is_turret(firer))
        return;
    auto* lpe = rf::local_player_entity;
    if (!lpe || !rf::entity_is_on_turret(lpe))
        return;
    gamepad_play_rumble(k_rumble_turret_shot);
}
