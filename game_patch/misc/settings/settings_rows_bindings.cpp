#include "settings_rows.h"

namespace
{

const SettingRow rows[] = {
    {
        .id = "bindings",
        .key = "Bind",
        .label = "Key bindings",
        .tooltip = "Assign keys and mouse buttons to actions.",
        .category = Cat::Bindings,
        .subcategory = "Bindings",
        .widget = Wid::Keybind,
        .access = AccNone{},
        .flags = SF_BESPOKE_INI | SF_NEEDS_LOCAL_PLAYER,
    },
    {
        .id = "bindings_reset",
        .label = "Reset bindings",
        .tooltip = "Restore the factory key bindings.",
        .category = Cat::Bindings,
        .subcategory = "Bindings",
        .widget = Wid::Action,
        .access = AccNone{},
        .flags = SF_NEEDS_LOCAL_PLAYER,
        .persistence = Pers::None,
    },
};

} // namespace

std::span<const SettingRow> settings_rows_bindings()
{
    return {rows, std::size(rows)};
}
