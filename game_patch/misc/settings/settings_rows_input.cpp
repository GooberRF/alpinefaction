#include <algorithm>
#include "../../input/mouse.h"
#include "settings_rows.h"

namespace
{

const char* const labels_mouse_scale[] = {"Classic", "Raw", "Modern"};

const SettingRow rows[] = {
    {
        .id = "swap_ar_controls",
        .key = "SwapARBinds",
        .label = "Swap AR fire modes",
        .tooltip = "Swap primary and alternate fire for the assault rifle.",
        .category = Cat::Input,
        .subcategory = "Weapon fire modes",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::swap_ar_controls},
        .console_cmd = "cl_swaparcontrols",
    },
    {
        .id = "swap_gn_controls",
        .key = "SwapGNBinds",
        .label = "Swap grenade fire modes",
        .tooltip = "Swap primary and alternate fire for grenades.",
        .category = Cat::Input,
        .subcategory = "Weapon fire modes",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::swap_gn_controls},
        .console_cmd = "cl_swapgrenadecontrols",
    },
    {
        .id = "swap_sg_controls",
        .key = "SwapSGBinds",
        .label = "Swap shotgun fire modes",
        .tooltip = "Swap primary and alternate fire for the shotgun.",
        .category = Cat::Input,
        .subcategory = "Weapon fire modes",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::swap_sg_controls},
        .console_cmd = "cl_swapsgcontrols",
    },
    {
        .id = "mouse_sensitivity",
        .key = "MouseSensitivity",
        .label = "Mouse sensitivity",
        .tooltip = "How fast the view turns with mouse movement.",
        .category = Cat::Input,
        .subcategory = "Mouse",
        .widget = Wid::Slider,
        .access = AccFnFloat{
            [](rf::Player* p) {
                auto* pl = ctx_player(p);
                return pl ? pl->settings.controls.mouse_sensitivity : 0.2f;
            },
            [](rf::Player* p, float v) {
                // A negative sensitivity would invert the view.
                if (auto* pl = ctx_player(p)) {
                    pl->settings.controls.mouse_sensitivity = std::max(v, 0.0f);
                }
            }},
        .flags = SF_NEEDS_LOCAL_PLAYER,
        .console_cmd = "ms",
        .default_str = "0.2",
        .ui_min = 0.0f,
        .ui_max = 0.5f,
        .ui_nudge = 0.01f,
        .ui_decimals = 5,
    },
    {
        .id = "mouse_y_invert",
        .key = "MouseYInvert",
        .label = "Invert mouse Y",
        .tooltip = "Invert vertical mouse movement.",
        .category = Cat::Input,
        .subcategory = "Mouse",
        .widget = Wid::Toggle,
        .access = AccFnBool{
            [](rf::Player* p) {
                auto* pl = ctx_player(p);
                return pl ? pl->settings.controls.axes[1].invert : false;
            },
            [](rf::Player* p, bool v) {
                if (auto* pl = ctx_player(p)) {
                    pl->settings.controls.axes[1].invert = v;
                }
            }},
        .flags = SF_NEEDS_LOCAL_PLAYER,
        .default_str = "0",
    },
    {
        .id = "mouse_scale",
        .key = "MouseScale",
        .label = "Mouse input model",
        .tooltip = "How raw mouse motion converts to view rotation.",
        .category = Cat::Input,
        .subcategory = "Mouse",
        .widget = Wid::Enum,
        .access = AccInt{&AGS::mouse_scale, nullptr, 0, 2},
        .apply = apply_mouse_scale,
        ENUM_LABELS(labels_mouse_scale),
        .console_cmd = "ms_scale",
    },
    {
        .id = "direct_input",
        .key = "DirectInput",
        .label = "DirectInput mouse",
        .tooltip = "Read the mouse through DirectInput instead of window messages.",
        .category = Cat::Input,
        .subcategory = "Mouse",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::direct_input},
        .apply = apply_direct_input,
        .console_cmd = "inputmode",
    },
    {
        .id = "mouse_linear_pitch",
        .key = "MouseLinearPitch",
        .label = "Linear pitch",
        .tooltip = "Make vertical mouse movement feel linear across the full pitch range.",
        .category = Cat::Input,
        .subcategory = "Mouse",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::mouse_linear_pitch},
        .console_cmd = "cl_linearpitch",
    },
    {
        .id = "background_mouse",
        .key = "EnableBackgroundMouse",
        .label = "Mouse when unfocused",
        .tooltip = "Keep processing mouse input while the window is unfocused.",
        .category = Cat::Input,
        .subcategory = "Mouse",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::background_mouse},
        .flags = SF_HIDDEN,
    },
    {
        .id = "scope_static_sensitivity",
        .key = "StaticScopeSensitivity",
        .label = "Fixed scope sensitivity",
        .tooltip = "Use one fixed scope sensitivity instead of scaling with zoom.",
        .category = Cat::Input,
        .subcategory = "Mouse",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::scope_static_sensitivity},
        .flags = SF_ADVANCED,
        .console_cmd = "cl_staticscopesens",
    },
    {
        .id = "scope_sensitivity_modifier",
        .key = "ScopeSensitivityModifier",
        .label = "Scope sensitivity",
        .tooltip = "Mouse sensitivity multiplier while zoomed.",
        .category = Cat::Input,
        .subcategory = "Mouse",
        .widget = Wid::Slider,
        .access = AccFloat{&AGS::scope_sensitivity_modifier, &AGS::set_scope_sens_mod,
            AGS::min_sens_mod, AGS::max_sens_mod},
        .flags = SF_APPLY_ON_LOAD,
        .apply = update_scope_sensitivity,
        .console_cmd = "cl_scopesens",
        .ui_min = 0.1f,
        .ui_max = 3.0f,
        .ui_nudge = 0.05f,
        .ui_decimals = 5,
    },
    {
        .id = "scanner_sensitivity_modifier",
        .key = "ScannerSensitivityModifier",
        .label = "Scanner sensitivity",
        .tooltip = "Mouse sensitivity multiplier in the rail driver scanner.",
        .category = Cat::Input,
        .subcategory = "Mouse",
        .widget = Wid::Slider,
        .access = AccFloat{&AGS::scanner_sensitivity_modifier, &AGS::set_scanner_sens_mod,
            AGS::min_sens_mod, AGS::max_sens_mod},
        .flags = SF_APPLY_ON_LOAD,
        .apply = update_scanner_sensitivity,
        .console_cmd = "cl_scannersens",
        .ui_min = 0.1f,
        .ui_max = 3.0f,
        .ui_nudge = 0.05f,
        .ui_decimals = 5,
    },
    {
        .id = "toggle_crouch",
        .key = "ToggleCrouch",
        .label = "Toggle crouch",
        .tooltip = "Press once to crouch, again to stand.",
        .category = Cat::Input,
        .subcategory = "Keyboard",
        .widget = Wid::Toggle,
        .access = PS_BOOL(toggle_crouch, false),
        .flags = SF_NEEDS_LOCAL_PLAYER,
        .console_cmd = "cl_togglecrouch",
        .default_str = "0",
    },
    {
        .id = "skip_cutscene_bind_alias",
        .key = "SkipCutsceneBindAlias",
        .label = "Extra skip cutscene key",
        .tooltip = "Also use this action's key to skip cutscenes.",
        .category = Cat::Input,
        .subcategory = "Keyboard",
        .widget = Wid::Enum,
        .access = AccInt{&AGS::skip_cutscene_bind_alias, nullptr, -1, 127},
        .flags = SF_SP_ONLY | SF_ACTION_CHOICE | SF_ADVANCED,
    },
    {
        .id = "quick_exit",
        .key = "QuickExit",
        .label = "Shift+Esc quits",
        .tooltip = "Allow Shift+Esc to exit the game instantly.",
        .category = Cat::Input,
        .subcategory = "Keyboard",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::quick_exit},
        .console_cmd = "key_quick_exit",
    },
    {
        .id = "mouse_look",
        .label = "Mouse look",
        .tooltip = "Turn the view with the mouse.",
        .category = Cat::Input,
        .subcategory = "Keyboard",
        .widget = Wid::Toggle,
        .access = AccFnBool{
            [](rf::Player* p) {
                auto* pl = ctx_player(p);
                return pl ? pl->settings.controls.mouse_look : true;
            },
            [](rf::Player* p, bool v) {
                if (auto* pl = ctx_player(p)) {
                    pl->settings.controls.mouse_look = v;
                }
            }},
        .flags = SF_HIDDEN | SF_NEEDS_LOCAL_PLAYER,
        .persistence = Pers::None,
        .default_str = "1",
    },
    {
        .id = "controller_placeholder",
        .label = "Controller",
        .tooltip = "Controller input is not available in this build.",
        .category = Cat::Input,
        .subcategory = "Controller",
        .widget = Wid::Label,
        .access = AccNone{},
        .flags = SF_READ_ONLY,
        .persistence = Pers::None,
    },
    {
        .id = "vr_placeholder",
        .label = "VR",
        .tooltip = "VR input is not available in this build.",
        .category = Cat::Input,
        .subcategory = "VR",
        .widget = Wid::Label,
        .access = AccNone{},
        .flags = SF_READ_ONLY,
        .persistence = Pers::None,
    },
};

} // namespace

std::span<const SettingRow> settings_rows_input()
{
    return {rows, std::size(rows)};
}
