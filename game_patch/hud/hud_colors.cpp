#include <common/utils/string-utils.h>
#include <optional>
#include "../misc/alpine_options.h"
#include "../misc/alpine_settings.h"
#include "../os/console.h"

void print_hex_color_state(
    const std::string& label,
    const std::optional<uint32_t>& value)
{
    if (value) {
        auto [r, g, b, a] = extract_color_components(*value);
        rf::console::print("{} is {} (RGBA {}, {}, {}, {})", label, format_hex_color_string(*value), r, g, b, a);
    }
    else {
        rf::console::print("{} is not set.", label);
    }
}

bool handle_hex_color_console_command(
    const std::optional<std::string>& input,
    const std::string& label,
    std::optional<uint32_t>& target)
{
    if (!input) {
        print_hex_color_state(label, target);
        return false;
    }

    auto normalized_input = string_to_lower(*input);
    if (normalized_input == "clear" ||
        normalized_input == "default" ||
        normalized_input == "-1") {
        target.reset();
        rf::console::print("{} cleared.", label);
        return false;
    }

    auto parsed_color = parse_hex_color_string(*input);
    if (!parsed_color) {
        rf::console::print(
            "Invalid input. Specify 'clear' to remove override. Specify override color as hex RRGGBB / RRGGBBAA or comma-separated RGB[A] values enclosed in quotes."
        );
        return false;
    }

    target = parsed_color;
    auto [r, g, b, a] = extract_color_components(*parsed_color);
    rf::console::print("{} set to {}, (RGBA {}, {}, {}, {})", label, format_hex_color_string(*parsed_color), r, g, b, a);
    return true;
}

void warn_if_overridden_by_alpine_option(
    const std::string& label,
    std::optional<AlpineOptionID> overriding_option)
{
    if (overriding_option && g_alpine_options_config.is_option_loaded(*overriding_option)) {
        rf::console::print("{} will be ignored because it is already overridden by an installed clientside mod.", label);
    }
}

ConsoleCommand2 sniper_scope_color_cmd{
    "ui_color_sniper_scope",
    [](std::optional<std::string> color_opt) {
        bool set_opt = handle_hex_color_console_command(
            color_opt,
            "Sniper scope color override",
            g_alpine_game_config.sniper_scope_color_override);

        if (set_opt) {
            warn_if_overridden_by_alpine_option(
                "Sniper scope color override",
                AlpineOptionID::SniperRifleScopeColor);
        }
    },
    "Set sniper scope color override.",
    "ui_color_sniper_scope <RRGGBB|RRGGBBAA|clear>",
};

ConsoleCommand2 precision_scope_color_cmd{
    "ui_color_precision_scope",
    [](std::optional<std::string> color_opt) {
        bool set_opt = handle_hex_color_console_command(
            color_opt,
            "Precision rifle scope color override",
            g_alpine_game_config.precision_scope_color_override);

        if (set_opt) {
            warn_if_overridden_by_alpine_option(
                "Precision rifle scope color override",
                AlpineOptionID::PrecisionRifleScopeColor);
        }
    },
    "Set precision rifle scope color override.",
    "ui_color_precision_scope <RRGGBB|RRGGBBAA|clear>",
};

ConsoleCommand2 rail_scope_color_cmd{
    "ui_color_rail_scope",
    [](std::optional<std::string> color_opt) {
        bool set_opt = handle_hex_color_console_command(
            color_opt,
            "Rail driver scope color override",
            g_alpine_game_config.rail_scope_color_override);

        if (set_opt) {
            warn_if_overridden_by_alpine_option(
                "Rail driver scope color override",
                AlpineOptionID::RailDriverScannerColor);
        }
    },
    "Set rail driver scope color override.",
    "ui_color_rail_scope <RRGGBB|RRGGBBAA|clear>",
};

ConsoleCommand2 ar_ammo_color_cmd{
    "ui_color_ar_ammo",
    [](std::optional<std::string> color_opt) {
        bool set_opt = handle_hex_color_console_command(
            color_opt,
            "Assault rifle ammo digit color override",
            g_alpine_game_config.ar_ammo_digit_color_override);

        if (set_opt) {
            warn_if_overridden_by_alpine_option(
                "Assault rifle ammo digit color override",
                AlpineOptionID::AssaultRifleAmmoColor);
        }
    },
    "Set assault rifle ammo digit color override.",
    "ui_color_ar_ammo <RRGGBB|RRGGBBAA|clear>",
};

ConsoleCommand2 damage_notify_color_cmd{
    "ui_color_damage_notify",
    [](std::optional<std::string> color_opt) {
        handle_hex_color_console_command(
            color_opt,
            "Damage notification color override",
            g_alpine_game_config.damage_notify_color_override);
    },
    "Set damage notification color override.",
    "ui_color_damage_notify <RRGGBB|RRGGBBAA|clear>",
};

ConsoleCommand2 location_ping_color_cmd{
    "ui_color_location_ping",
    [](std::optional<std::string> color_opt) {
        handle_hex_color_console_command(
            color_opt,
            "Location ping color override",
            g_alpine_game_config.location_ping_color_override);
    },
    "Set location ping color override.",
    "ui_color_location_ping <RRGGBB|RRGGBBAA|clear>",
};

ConsoleCommand2 multi_timer_color_cmd{
    "ui_color_multi_timer",
    [](std::optional<std::string> color_opt) {
        bool set_opt = handle_hex_color_console_command(
            color_opt,
            "Multiplayer timer color override",
            g_alpine_game_config.multi_timer_color_override);

        if (set_opt) {
            warn_if_overridden_by_alpine_option(
                "Multiplayer timer color override",
                AlpineOptionID::MultiTimerColor);
            multi_hud_update_timer_color();
        }
    },
    "Set multiplayer timer color override.",
    "ui_color_multi_timer <RRGGBB|RRGGBBAA|clear>",
};

ConsoleCommand2 teammate_label_color_cmd{
    "ui_color_team_label",
    [](std::optional<std::string> color_opt) {
        handle_hex_color_console_command(
            color_opt,
            "Teammate world label color override",
            g_alpine_game_config.teammate_label_color_override);
    },
    "Set teammate world label color override.",
    "ui_color_team_label <RRGGBB|RRGGBBAA|clear>",
};

ConsoleCommand2 reticle_color_cmd{
    "ui_color_reticle",
    [](std::optional<std::string> color_opt) {
        handle_hex_color_console_command(
            color_opt,
            "Reticle color override",
            g_alpine_game_config.reticle_color_override);

        if (g_alpine_game_config.reticle_color_override) {
            rf::console::print("Color override is {}being applied to custom reticles. Use ui_colorize_custom_reticles to change this behavior.", g_alpine_game_config.colorize_custom_reticles ? "" : "NOT ");
        }
    },
    "Set reticle color override.",
    "ui_color_reticle <RRGGBB|RRGGBBAA|clear>",
};

ConsoleCommand2 reticle_locked_color_cmd{
    "ui_color_reticle_locked",
    [](std::optional<std::string> color_opt) {
        handle_hex_color_console_command(
            color_opt,
            "Locked reticle color override",
            g_alpine_game_config.reticle_locked_color_override);

        if (g_alpine_game_config.reticle_locked_color_override) {
            rf::console::print("Color override is {}being applied to custom reticles. Use ui_colorize_custom_reticles to change this behavior.", g_alpine_game_config.colorize_custom_reticles ? "" : "NOT ");
        }
    },
    "Set locked reticle color override (SP only).",
    "ui_color_reticle_locked <RRGGBB|RRGGBBAA|clear>",
};

ConsoleCommand2 reticle_color_custom_cmd{
    "ui_colorize_custom_reticles",
    [](std::optional<std::string> color_opt) {
        g_alpine_game_config.colorize_custom_reticles = !g_alpine_game_config.colorize_custom_reticles;
        rf::console::print(
            "Colorization of custom reticle textures is {}",
            g_alpine_game_config.colorize_custom_reticles ? "enabled" : "disabled"
        );
    },
    "Should reticle color overrides be applied to custom reticles? This will produce undesirable effects with non-greyscale reticle textures",
    "ui_colorize_custom_reticles",
};

void hud_colors_apply_patch()
{
    sniper_scope_color_cmd.register_cmd();
    precision_scope_color_cmd.register_cmd();
    rail_scope_color_cmd.register_cmd();
    ar_ammo_color_cmd.register_cmd();
    damage_notify_color_cmd.register_cmd();
    location_ping_color_cmd.register_cmd();
    multi_timer_color_cmd.register_cmd();
    teammate_label_color_cmd.register_cmd();
    reticle_color_cmd.register_cmd();
    reticle_locked_color_cmd.register_cmd();
    reticle_color_custom_cmd.register_cmd();
}
