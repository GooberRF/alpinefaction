#pragma once

#include "../rf/player/control_config.h"

// Returns true if `action` must be reported as not pressed / not down for `ccp`.
using ControlInputVetoFn = bool (*)(rf::ControlConfig* ccp, rf::ControlConfigAction action);

// Synthetic input laid over the engine's own reading.
struct ControlInputInjection
{
    bool down = false;
    bool just_pressed = false;
};

using ControlInputInjectionFn = ControlInputInjection (*)(rf::ControlConfig* ccp, rf::ControlConfigAction action);

// Returns true if `action` must be reported as held down for `ccp`.
using ControlInputDownFn = bool (*)(rf::ControlConfig* ccp, rf::ControlConfigAction action);

// Installs both hooks. Called once at startup like the rest of game_patch/input.
void control_input_filter_apply_patch();

// Registration is independent of installation: a participant registers when its
// subsystem first becomes relevant, and the filter simply has nothing to do
// until then. Callers are responsible for registering only once.
void control_input_filter_add_veto(ControlInputVetoFn veto);

// A press injection applies to control_config_check_pressed only; a down injection to
// control_is_control_down only. A veto overrides both.
void control_input_filter_add_press_injection(ControlInputInjectionFn injection);
void control_input_filter_add_down_injection(ControlInputDownFn injection);

// control_config_check_pressed with every veto and injection bypassed, for a
// participant that needs the engine's raw reading of an action it itself vetoes.
bool control_input_filter_check_pressed_unfiltered(
    rf::ControlConfig* ccp,
    rf::ControlConfigAction action,
    bool* just_pressed);
