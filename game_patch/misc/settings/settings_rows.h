#pragma once

#include <algorithm>
#include <iterator>
#include <span>
#include <type_traits>
#include "../settings_registry.h"
#include "../../main/main.h"
#include "../../rf/player/player.h"

using Cat = SettingCategory;
using Wid = SettingWidget;
using Pers = SettingPersistence;
using SPerm = ServerPermission;
using AGS = AlpineGameSettings;
using ASC = AlpineCoreConfig;

inline rf::Player* ctx_player(rf::Player* player)
{
    return player ? player : rf::local_player;
}

// Accessors for stock PlayerSettings fields. `def` is what a caller sees while no player
// exists (settings can be queried from the console before the player is created).
// These aren't a perfect STYLE.md match, but they're concise and work.
// Goober todo: Revisit this approach.
#define PS_BOOL(field, def)                                                           \
    AccFnBool                                                                         \
    {                                                                                 \
        [](rf::Player* p) {                                                           \
            auto* pl = ctx_player(p);                                                 \
            return pl ? static_cast<bool>(pl->settings.field) : (def);                \
        },                                                                            \
        [](rf::Player* p, bool v) {                                                   \
            if (auto* pl = ctx_player(p)) {                                           \
                pl->settings.field = v;                                               \
            }                                                                         \
        }                                                                             \
    }

#define PS_INT(field, def, lo, hi)                                                    \
    AccFnInt                                                                          \
    {                                                                                 \
        [](rf::Player* p) {                                                           \
            auto* pl = ctx_player(p);                                                 \
            return pl ? static_cast<int>(pl->settings.field) : (def);                 \
        },                                                                            \
        [](rf::Player* p, int v) {                                                    \
            if (auto* pl = ctx_player(p)) {                                           \
                pl->settings.field = v;                                               \
            }                                                                         \
        },                                                                            \
        (lo), (hi)                                                                    \
    }

#define ENUM_LABELS(arr) .enum_labels = (arr), .enum_count = static_cast<int>(std::size(arr))

std::span<const SettingRow> settings_rows_graphics();
std::span<const SettingRow> settings_rows_hud();
std::span<const SettingRow> settings_rows_interface();
std::span<const SettingRow> settings_rows_audio();
std::span<const SettingRow> settings_rows_bindings();
std::span<const SettingRow> settings_rows_input();
std::span<const SettingRow> settings_rows_gameplay();
std::span<const SettingRow> settings_rows_vehicles();
std::span<const SettingRow> settings_rows_multiplayer();
std::span<const SettingRow> settings_rows_spectator();
