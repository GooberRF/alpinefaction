#include "rumble.h"
#include "gamepad.h"
#include "../rf/entity.h"
#include "../rf/weapon.h"
#include "../misc/alpine_settings.h"
#include <algorithm>

// Weapons that should never produce rumble even when last_fired_timestamp advances.
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
            gamepad_weapon_fire_rumble(0x2000, 0xC000, 0x6000, 90u);
        s_last_fired_ts = cur_fired;
        return;
    }

    if (!rumble_weapon_is_skipped(wt, winfo)
            && s_last_fired_ts != -2
            && cur_fired != s_last_fired_ts) {

        float dmg   = winfo.damage > 0.0f ? winfo.damage : winfo.alt_damage;
        float scale = std::min(dmg / 200.0f, 1.0f);

        bool continuous = (winfo.flags & (rf::WTF_CONTINUOUS_FIRE | rf::WTF_ALT_CONTINUOUS_FIRE)) != 0;
        bool burst      = (winfo.flags & rf::WTF_BURST_MODE) != 0;

        uint32_t duration      = continuous ? 70u : burst ? 55u : 100u;
        uint16_t lo_motor      = static_cast<uint16_t>(scale * 0x8000);
        uint16_t hi_motor      = static_cast<uint16_t>(scale * 0x6000);
        uint16_t trigger_motor = static_cast<uint16_t>(scale * 0xA000);

        gamepad_weapon_fire_rumble(lo_motor, hi_motor, trigger_motor, duration);
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
    uint16_t lo, hi;
    uint32_t duration_ms;

    if (damage_type == rf::DT_BASH) {
        // Melee hit: sharp, high-frequency dominant impact
        float scale = std::min(damage / 40.0f, 1.0f);
        lo          = static_cast<uint16_t>(scale * 0x3000);
        hi          = static_cast<uint16_t>(scale * 0x8000);
        duration_ms = 120u;
    }
    else if (damage_type == rf::DT_EXPLOSIVE) {
        // Explosion: strong low-freq rumble, scales down only on small splash damage
        float scale = std::min(damage / 75.0f, 1.0f);
        lo          = static_cast<uint16_t>(scale * 0xC000);
        hi          = static_cast<uint16_t>(scale * 0xA000);
        duration_ms = 300u;
    }
    else if (damage_type == rf::DT_FIRE) {
        // Flame grenade / fire damage: buzzy high-freq with moderate rumble
        float scale = std::min(damage / 20.0f, 1.0f);
        lo          = static_cast<uint16_t>(scale * 0x3000);
        hi          = static_cast<uint16_t>(scale * 0x9000);
        duration_ms = 150u;
    }
    else {
        return;
    }

    gamepad_rumble(lo, hi, duration_ms);
}
