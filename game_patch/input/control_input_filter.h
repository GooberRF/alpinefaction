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

// Installs both hooks. Called once at startup like the rest of game_patch/input.
void control_input_filter_apply_patch();

// Registration is independent of installation: a participant registers when its
// subsystem first becomes relevant, and the filter simply has nothing to do
// until then. Callers are responsible for registering only once.
void control_input_filter_add_veto(ControlInputVetoFn veto);

// Injection applies to control_config_check_pressed only, matching the single
// function bots used to hook. Nothing injects into control_is_control_down.
void control_input_filter_add_press_injection(ControlInputInjectionFn injection);

// control_config_check_pressed with every veto and injection bypassed, for a
// participant that needs the engine's raw reading of an action it itself vetoes.
bool control_input_filter_check_pressed_unfiltered(
    rf::ControlConfig* ccp,
    rf::ControlConfigAction action,
    bool* just_pressed);
