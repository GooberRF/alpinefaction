#include <array>
#include <cstddef>
#include <patch_common/FunHook.h>
#include <xlog/xlog.h>
#include "control_input_filter.h"
#include "../rf/player/control_config.h"

namespace
{

// Three participants exist today (vote panel, waypoint editor, client bots). The
// cap only exists so registration needs no allocation and the hooks stay a flat
// walk over a fixed array.
constexpr std::size_t max_participants = 8;

std::array<ControlInputVetoFn, max_participants> g_vetoes{};
std::size_t g_veto_count = 0;

std::array<ControlInputInjectionFn, max_participants> g_injections{};
std::size_t g_injection_count = 0;

// Vetoes are OR-ed, so their registration order does not matter.
bool action_is_vetoed(rf::ControlConfig* ccp, rf::ControlConfigAction action)
{
    for (std::size_t i = 0; i < g_veto_count; ++i) {
        if (g_vetoes[i](ccp, action)) {
            return true;
        }
    }
    return false;
}

ControlInputInjection resolve_injection(rf::ControlConfig* ccp, rf::ControlConfigAction action)
{
    ControlInputInjection result{};
    for (std::size_t i = 0; i < g_injection_count; ++i) {
        const ControlInputInjection injected = g_injections[i](ccp, action);
        result.down = result.down || injected.down;
        result.just_pressed = result.just_pressed || injected.just_pressed;
    }
    return result;
}

FunHook<bool(rf::ControlConfig*, rf::ControlConfigAction, bool*)> control_config_check_pressed_hook{
    0x0043D4F0,
    [](rf::ControlConfig* ccp, rf::ControlConfigAction action, bool* just_pressed) {
        // A veto owns the action completely: the engine is not consulted and no
        // injection can lift it.
        if (action_is_vetoed(ccp, action)) {
            if (just_pressed) {
                *just_pressed = false;
            }
            return false;
        }

        const bool pressed = control_config_check_pressed_hook.call_target(ccp, action, just_pressed);

        const ControlInputInjection injected = resolve_injection(ccp, action);
        if (!injected.down) {
            return pressed;
        }
        if (just_pressed) {
            *just_pressed = *just_pressed || injected.just_pressed;
        }
        return true;
    },
};

FunHook<bool(rf::ControlConfig*, rf::ControlConfigAction)> control_is_control_down_hook{
    0x00430F40,
    [](rf::ControlConfig* ccp, rf::ControlConfigAction action) {
        if (action_is_vetoed(ccp, action)) {
            return false;
        }
        // Deliberately no injection: bots drive fire through
        // control_config_check_pressed only, which is the one function their own
        // hook ever covered.
        return control_is_control_down_hook.call_target(ccp, action);
    },
};

} // namespace

void control_input_filter_apply_patch()
{
    control_config_check_pressed_hook.install();
    control_is_control_down_hook.install();
}

void control_input_filter_add_veto(ControlInputVetoFn veto)
{
    if (!veto) {
        return;
    }
    if (g_veto_count >= g_vetoes.size()) {
        xlog::error("control input filter: veto registration overflow, input policy will be wrong");
        return;
    }
    g_vetoes[g_veto_count++] = veto;
}

void control_input_filter_add_press_injection(ControlInputInjectionFn injection)
{
    if (!injection) {
        return;
    }
    if (g_injection_count >= g_injections.size()) {
        xlog::error("control input filter: injection registration overflow, input policy will be wrong");
        return;
    }
    g_injections[g_injection_count++] = injection;
}

bool control_input_filter_check_pressed_unfiltered(
    rf::ControlConfig* ccp,
    rf::ControlConfigAction action,
    bool* just_pressed)
{
    return control_config_check_pressed_hook.call_target(ccp, action, just_pressed);
}
