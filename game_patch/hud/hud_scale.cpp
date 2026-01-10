#include <charconv>
#include <cstdlib>
#include <optional>
#include <string>
#include <common/utils/string-utils.h>
#include "../misc/alpine_settings.h"
#include "../os/console.h"
#include "hud_internal.h"

enum class UIScaleElement
{
    DamageNotify,
    PlayerLabel,
    PingLabel,
    Reticle
};

struct UIScaleCommandContext
{
    std::string_view label;
    std::optional<float>& target;
};

UIScaleCommandContext get_ui_scale_context(UIScaleElement element)
{
    switch (element) {
        case UIScaleElement::DamageNotify:
            return {
                "Damage notification text",
                g_alpine_game_config.world_hud_damage_text_scale,
            };
        case UIScaleElement::PlayerLabel:
            return {
                "Player label text",
                g_alpine_game_config.world_hud_label_text_scale,
            };
        case UIScaleElement::PingLabel:
            return {
                "Location ping label text",
                g_alpine_game_config.world_hud_ping_label_text_scale,
            };
        case UIScaleElement::Reticle:
            return {
                "Reticle",
                g_alpine_game_config.reticle_scale,
            };
        default:
            static std::optional<float> unused_target{};
            return {"unknown", unused_target};
    }
}

void set_ui_scale_value(UIScaleElement element, float scale)
{
    switch (element) {
        case UIScaleElement::DamageNotify:
            g_alpine_game_config.set_world_hud_damage_text_scale(scale);
            return;
        case UIScaleElement::PlayerLabel:
            g_alpine_game_config.set_world_hud_label_text_scale(scale);
            return;
        case UIScaleElement::PingLabel:
            g_alpine_game_config.set_world_hud_ping_label_text_scale(scale);
            return;
        case UIScaleElement::Reticle:
            g_alpine_game_config.set_reticle_scale(scale);
            return;
        default:
            return;
    }
}

void clear_ui_scale_value(UIScaleElement element)
{
    switch (element) {
        case UIScaleElement::DamageNotify:
            g_alpine_game_config.clear_world_hud_damage_text_scale();
            return;
        case UIScaleElement::PlayerLabel:
            g_alpine_game_config.clear_world_hud_label_text_scale();
            return;
        case UIScaleElement::PingLabel:
            g_alpine_game_config.clear_world_hud_ping_label_text_scale();
            return;
        case UIScaleElement::Reticle:
            g_alpine_game_config.clear_reticle_scale();
            return;
        default:
            return;
    }
}

void print_ui_scale_state(const std::string_view& label, const std::optional<float>& value)
{
    if (value) {
        rf::console::print("{} scale override is {:.2f}", label, *value);
    }
    else {
        rf::console::print("{} scale is default (1.00)", label);
    }
}

bool handle_ui_scale_console_command(
    const std::optional<std::string>& input,
    UIScaleElement element)
{
    const auto context = get_ui_scale_context(element);

    if (!input) {
        print_ui_scale_state(context.label, context.target);
        return false;
    }

    auto normalized_input = string_to_lower(*input);
    if (normalized_input == "clear" ||
        normalized_input == "default" ||
        normalized_input == "-1") {
        clear_ui_scale_value(element);
        print_ui_scale_state(context.label, context.target);
        return false;
    }

    float parsed_scale = 0.0f;
    const auto* begin = input->data();
    const auto* end = begin + input->size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed_scale);
    if (ec != std::errc{} || ptr != end) {
        rf::console::print("Invalid input. Specify 'clear' to remove override. Specify override scale as a numeric value.");
        return false;
    }

    set_ui_scale_value(element, parsed_scale);
    print_ui_scale_state(context.label, context.target);
    return true;
}

ConsoleCommand2 damage_notify_scale_cmd{
    "ui_scale_damage_notify",
    [](std::optional<std::string> scale_opt) {
        handle_ui_scale_console_command(scale_opt, UIScaleElement::DamageNotify);
    },
    "Scale damage notification text.",
    "ui_scale_damage_notify <scale|clear>",
};

ConsoleCommand2 player_label_scale_cmd{
    "ui_scale_player_label",
    [](std::optional<std::string> scale_opt) {
        handle_ui_scale_console_command(scale_opt, UIScaleElement::PlayerLabel);
    },
    "Scale player label text.",
    "ui_scale_player_label <scale|clear>",
};

ConsoleCommand2 ping_label_scale_cmd{
    "ui_scale_ping_label",
    [](std::optional<std::string> scale_opt) {
        handle_ui_scale_console_command(scale_opt, UIScaleElement::PingLabel);
    },
    "Scale location ping label text.",
    "ui_scale_ping_label <scale|clear>",
};

ConsoleCommand2 reticle_scale_cmd{
    "ui_scale_reticle",
    [](std::optional<std::string> scale_opt) {
        handle_ui_scale_console_command(scale_opt, UIScaleElement::Reticle);
    },
    "Scale reticle.",
    "ui_scale_reticle <scale|clear>",
};

DcCommandAlias legacy_reticle_scale_cmd{
    "ui_reticlescale",
    reticle_scale_cmd
};

void hud_scale_apply_patch()
{
    damage_notify_scale_cmd.register_cmd();
    player_label_scale_cmd.register_cmd();
    ping_label_scale_cmd.register_cmd();
    reticle_scale_cmd.register_cmd();
    legacy_reticle_scale_cmd.register_cmd();
}
